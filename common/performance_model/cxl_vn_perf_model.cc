#include "cxl_vn_perf_model.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "vv_perf_model.h"

#if 0
#define MYTRACE_ENABLED
   extern Lock iolock;
#include "core_manager.h"
#include "simulator.h"
#define MYTRACE(...)                                    \
{                                                       \
    ScopedLock l(iolock);                               \
    fflush(f_trace);                                    \
    fprintf(f_trace, "[%d VN] ", m_cxl_id);            \
    fprintf(f_trace, __VA_ARGS__);                      \
    fprintf(f_trace, "\n");                             \
    fflush(f_trace);                                    \
}
#else
#define MYTRACE(...) {}
#endif

CXLVNPerfModel::CXLVNPerfModel(cxl_id_t cxl_id, UInt64 transaction_size /* in bits */,
    QueueModel* shared_bus_queue, ComponentBandwidth* shared_bus_bw):
    CXLPerfModel(cxl_id, transaction_size),
    m_queue_model(shared_bus_queue),
    m_bus_bw(shared_bus_bw),
    m_owns_queue(false),
    m_cxl_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/cxl/vnserver/bandwidth")),
    m_total_queueing_delay(SubsecondTime::Zero()),
    m_total_access_latency(SubsecondTime::Zero()),
    m_vv_perf_model(NULL),
    m_throttle_enabled(false),
    m_throttle_period_per_update(SubsecondTime::Zero()),
    m_throttle_hits(0),
    m_throttle_delay_ns(0)
{
    m_vv_perf_model = VVPerfModel::createVVPerfModel(cxl_id, transaction_size);
    m_cxl_access_cost =
        SubsecondTime::FS() *
        static_cast<uint64_t>(
            TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat(
                "perf_model/cxl/vnserver/latency")));
    if (!m_queue_model) {
        // Fallback: create own queue if no shared queue provided
        m_queue_model = QueueModel::create("cxl-queue", cxl_id,
            Sim()->getCfg()->getString("perf_model/cxl/queue_type"),
            m_cxl_bandwidth.getRoundedLatency(transaction_size));
        m_bus_bw = &m_cxl_bandwidth;
        m_owns_queue = true;
    }

    // Optional dynamic throttle (Run E mitigation). Disabled by default so
    // Runs A–D produce identical results to the un-throttled baseline.
    try {
        m_throttle_enabled = Sim()->getCfg()->getBool("perf_model/cxl/vnserver/throttle/enable");
    } catch (...) {
        m_throttle_enabled = false;
    }
    if (m_throttle_enabled) {
        UInt64 rate_per_us = static_cast<UInt64>(
            Sim()->getCfg()->getInt("perf_model/cxl/vnserver/throttle/rate_per_us_per_core"));
        if (rate_per_us == 0) rate_per_us = 1;
        // 1 microsecond = 1000 ns;  period per update = 1000 ns / rate
        m_throttle_period_per_update = SubsecondTime::NS(1000) / rate_per_us;
    }

    registerStatsMetric("cxl", cxl_id, "total-access-latency", &m_total_access_latency);
    registerStatsMetric("cxl", cxl_id, "total-queueing-delay", &m_total_queueing_delay);
    registerStatsMetric("cxl", cxl_id, "throttle-hits", &m_throttle_hits);
    registerStatsMetric("cxl", cxl_id, "throttle-delay-ns", &m_throttle_delay_ns);

#ifdef MYTRACE_ENABLED
    std::ostringstream trace_filename;
    trace_filename << "vn_vault_perf_" << (int)m_cxl_id << ".trace";
    f_trace = fopen(trace_filename.str().c_str(), "w+");
    std::cerr << "Create VN Vault perf trace " << trace_filename.str().c_str() << std::endl;
#endif // MYTRACE_ENABLED
}

CXLVNPerfModel::~CXLVNPerfModel()
{
    if (m_owns_queue && m_queue_model)
    {
        delete m_queue_model;
        m_queue_model = NULL;
    }
    if (m_vv_perf_model)
    {
        delete m_vv_perf_model;
        m_vv_perf_model = NULL;
    }
}

SubsecondTime CXLVNPerfModel::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, CXLCntlrInterface::access_t access_type, ShmemPerf *perf)
{
    // pkt_size is in 'bits'
    // m_dram_bandwidth is in 'Bits per clock cycle'
    if ((!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()))
        return SubsecondTime::Zero();

    // ── Dynamic per-tenant throttle (Run E mitigation) ─────────────────────
    // Token-bucket rate limiter on VN_UPDATEs only, applied at the device
    // front-end before the request enters the shared CXL queue.  Reads pass
    // through unmodified; benign workloads with low write rates are unaffected.
    //
    // Per-access throttle delay feeds into access_latency unmodified so the
    // simulator's core model sees the full back-pressure.  The cumulative
    // throttle delay is recorded as a UInt64 nanosecond counter (separate
    // from the device's total-access-latency SubsecondTime, which would
    // overflow under sustained attack — see header comment).
    SubsecondTime throttle_delay = SubsecondTime::Zero();
    if (m_throttle_enabled && access_type == CXLCntlrInterface::VN_UPDATE) {
        auto it = m_throttle_next_allowed.find(requester);
        SubsecondTime allowed = (it != m_throttle_next_allowed.end()) ? it->second : SubsecondTime::Zero();
        if (pkt_time < allowed) {
            throttle_delay = allowed - pkt_time;
            pkt_time = allowed;
            m_throttle_hits++;
            m_throttle_delay_ns += throttle_delay.getNS();
        }
        m_throttle_next_allowed[requester] = pkt_time + m_throttle_period_per_update;
    }

    SubsecondTime vv_latency;
    boost::tie(vv_latency, pkt_size) = m_vv_perf_model->getAccessLatency(
        pkt_time, requester, address, access_type, perf);

    SubsecondTime processing_time = m_bus_bw->getRoundedLatency(pkt_size);

    // Compute Queue Delay (shared CXL bus — contends with data traffic)
    SubsecondTime queue_delay;
    queue_delay = m_queue_model->computeQueueDelay(pkt_time + vv_latency, processing_time, requester);

    SubsecondTime access_latency = throttle_delay + queue_delay + processing_time + vv_latency + m_cxl_access_cost;

    switch(access_type){
        case CXLCntlrInterface::VN_READ:
            MYTRACE("VN_R ==%s== @ %016lx", itostr(pkt_time + queue_delay + processing_time).c_str(), address);
            perf->updateTime(pkt_time + access_latency, ShmemPerf::VN_DEVICE);
            break;
        case CXLCntlrInterface::VN_UPDATE:
            MYTRACE("VN_U ==%s== @ %016lx", itostr(pkt_time + queue_delay + processing_time).c_str(), address);
            break;
        default:
            LOG_PRINT_ERROR("Unrecognized CXL access Type: %u", access_type);
            break;
    }

    // Update Memory Counters
    m_num_accesses++;
    // total-access-latency tracks the device's own service time (queue +
    // processing + DRAM + access cost), excluding the throttle wait. The
    // throttle wait is captured separately via throttle-delay-ns to keep
    // this stat's average bounded under sustained back-pressure.
    SubsecondTime device_latency = queue_delay + processing_time + vv_latency + m_cxl_access_cost;
    m_total_access_latency += device_latency;
    m_total_queueing_delay += queue_delay;

    return access_latency;
}

void CXLVNPerfModel::enable()
{
    m_enabled = true;
    m_vv_perf_model->enable();
}

void CXLVNPerfModel::disable() {
    m_enabled = false;
    m_vv_perf_model->disable();
}