#ifndef __CXL_VN_PERF_MODEL_H__
#define __CXL_VN_PERF_MODEL_H__

#include <unordered_map>
#include "cxl_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "cxl_cntlr_interface.h"
#include "vv_perf_model.h"

class CXLVNPerfModel : public CXLPerfModel
{
   private:
      QueueModel* m_queue_model;        // shared bus queue (not owned)
      ComponentBandwidth* m_bus_bw;     // shared bus bandwidth (not owned)
      bool m_owns_queue;
      SubsecondTime m_cxl_access_cost;
      ComponentBandwidth m_cxl_bandwidth; // kept for fallback

      SubsecondTime m_total_queueing_delay;
      SubsecondTime m_total_access_latency;

      VVPerfModel* m_vv_perf_model;

      // ── Dynamic per-tenant throttling (Run E mitigation) ───────────────────
      // Token-bucket rate limiter applied to VN_UPDATEs only.  When a core
      // exceeds its allowed update rate the request is delayed so subsequent
      // VN_UPDATEs from that core arrive at the device queue at most every
      // m_throttle_period_per_update.  Default: disabled (matches Run A–D).
      //
      // Accumulator note: under a sustained attack the cumulative throttle
      // delay can grow O(N²) (each new request inherits the debt of all
      // previous bursty requests). At N = 324 K updates with a 200 ns period,
      // the sum reaches ~1e19 fs — past Int64 max if stored in SubsecondTime
      // (which is fs internally).  We therefore (a) keep the per-access
      // SubsecondTime arithmetic exact for back-pressure correctness, and
      // (b) accumulate the cumulative throttle into a UInt64 nanosecond
      // counter for the stat, which gives ~5800-year head-room.
      bool m_throttle_enabled;
      SubsecondTime m_throttle_period_per_update;  // 1 / configured rate
      std::unordered_map<core_id_t, SubsecondTime> m_throttle_next_allowed;
      UInt64 m_throttle_hits;          // # of VN_UPDATEs that incurred a delay
      UInt64 m_throttle_delay_ns;      // cumulative throttle delay (nanoseconds)


     public:
      CXLVNPerfModel(cxl_id_t cxl_id, UInt64 cache_block_size /* in bits */,
          QueueModel* shared_bus_queue = NULL, ComponentBandwidth* shared_bus_bw = NULL);
      ~CXLVNPerfModel();
      SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size,
                                     core_id_t requester, IntPtr address,
                                     CXLCntlrInterface::access_t access_type,
                                     ShmemPerf* perf);
      void enable();
      void disable();

      UInt64 getTotalAccesses() { return m_num_accesses; }
};

#endif // __CXL_VN_PERF_MODEL_H__