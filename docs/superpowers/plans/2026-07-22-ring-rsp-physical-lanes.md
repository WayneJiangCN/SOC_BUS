# Ring RSP Physical Lanes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add configurable RSP physical lanes to the AI core ring bus model so `rd_rsp_port_num` remains a logical response-port concept while `rsp_phys_lanes` represents real parallel RSP link resources.

**Architecture:** Keep the existing shared ring router and existing REQ/RSP subnet split. Do not replicate the whole ring network like intersim2 `subnets`; instead, make each RSP link own multiple independent physical lanes, each with its own serialization state, inflight count, propagation FIFO, bubble reservation, and statistics. Existing RD/WR/WR_DAT target-side FIFOs remain split; REQ physical link stays single-lane in this phase.

**Tech Stack:** C++ ring ESL model under `BUS/aicore`, custom TOML parser in `tm_ring_demo_config.cc`, existing utest flow through `test_prj`, result-file based validation via `pem_multi_core_result.txt`.

## Global Constraints

- Scope is multi-core ring ESL only.
- Do not replace the ring topology or duplicate the whole router/network fabric.
- Keep `rd_rsp_port_num` as logical RD response port/channel count.
- Add `rsp_phys_lanes` as the only parameter that increases RSP physical link bandwidth.
- Default `rsp_phys_lanes = 1` must preserve current behavior as closely as possible.
- Keep REQ physical link single-lane in this phase.
- Keep existing RD_REQ/WR_REQ/WR_DAT target FIFO split; do not add another REQ refactor in this change.
- Keep existing `TEST_BOTTLENECK` aggregate fields backward-compatible; only append new fields/lines.
- Add Chinese comments in the TOML config for newly added parameters.

---

## File Structure

- Modify `BUS/aicore/tm/tm_pld.h`
  - Add carried physical RSP lane metadata.
- Modify `BUS/aicore/tm/tm_pld.cc`
  - Copy the new metadata in the payload copy constructor.
- Modify `BUS/aicore/tm_ring_types.h`
  - Add `TmRingRspLaneSelect`.
  - Add `rsp_phys_lanes` and `rsp_lane_select` to `TmRingCfg`.
  - Add helper to normalize physical lane count.
- Modify `BUS/aicore/tm_ring_demo_config.h`
  - Add demo config fields for `rsp_phys_lanes` and `rsp_lane_select`.
- Modify `BUS/aicore/tm_ring_demo_config.cc`
  - Parse TOML and option-string keys.
  - Validate lane count and lane selection.
- Modify `BUS/aicore/config/pem_config_cloud.toml`
  - Add documented default multi-core RSP physical-lane settings.
- Modify `BUS/aicore/tm_ring_demo_test.h`
  - Transfer demo config into ring config.
  - Print config and per-lane stats.
  - Adjust read peak calculation to account for RSP physical lanes.
- Modify `BUS/aicore/tm_ring_link.h`
  - Change per-subnet resources into per-subnet/per-physical-lane resources.
  - Add per-lane stat accessor while keeping aggregate accessor.
- Modify `BUS/aicore/tm_ring_link.cc`
  - Implement independent RSP physical lanes.
  - Preserve existing aggregate behavior by summing lanes.
- Modify `BUS/aicore/tm_ring_target_port.h`
  - Add lane-selection helper/state.
- Modify `BUS/aicore/tm_ring_target_port.cc`
  - Assign `ring_rsp_phys_lane` when responses are injected into the RSP subnet.
- Modify `BUS/aicore/tm_ring.h`
  - Add lane hotspot/stat structs.
  - Add query APIs for per-lane stats.
- Modify `BUS/aicore/tm_ring_core.cc`
  - Aggregate per-lane stats into existing totals.
  - Expose per-lane hotspots.

---

### Task 1: Add configuration semantics without changing behavior

**Files:**
- Modify: `BUS/aicore/tm_ring_types.h`
- Modify: `BUS/aicore/tm_ring_demo_config.h`
- Modify: `BUS/aicore/tm_ring_demo_config.cc`
- Modify: `BUS/aicore/config/pem_config_cloud.toml`
- Modify: `BUS/aicore/tm_ring_demo_test.h`

**Interfaces:**
- Produces:
  - `enum class TmRingRspLaneSelect { TARGET = 0, HASH = 1, ROUND_ROBIN = 2 };`
  - `TmRingCfg::rsp_phys_lanes`
  - `TmRingCfg::rsp_lane_select`
  - `RingDemoConfig::rsp_phys_lanes`
  - `RingDemoConfig::rsp_lane_select`

- [ ] **Step 1: Add ring-level config fields**

In `BUS/aicore/tm_ring_types.h`, add the enum near `TmRingSubnet`:

```cpp
enum class TmRingRspLaneSelect : uint32_t {
  TARGET = 0,
  HASH = 1,
  ROUND_ROBIN = 2,
};
```

In `struct TmRingCfg`, add:

```cpp
  // RSP 物理 lane 数；区别于 rd_rsp_port_num，后者只是逻辑响应通道数。
  uint32_t rsp_phys_lanes = 1;
  TmRingRspLaneSelect rsp_lane_select = TmRingRspLaneSelect::TARGET;
```

Add helper:

```cpp
inline constexpr uint32_t tm_ring_rsp_phys_lane_count(uint32_t lanes) {
  return lanes == 0 ? 1 : lanes;
}
```

- [ ] **Step 2: Add demo config fields**

In `BUS/aicore/tm_ring_demo_config.h`, add:

```cpp
    uint32_t rsp_phys_lanes = 1;
    TmRingRspLaneSelect rsp_lane_select = TmRingRspLaneSelect::TARGET;
```

Include `tm_ring_types.h` if needed.

- [ ] **Step 3: Parse TOML and command options**

In `BUS/aicore/tm_ring_demo_config.cc`, add option mappings:

```cpp
{"--rsp-phys-lanes", "rsp_phys_lanes"},
{"--rsp-lane-select", "rsp_lane_select"},
```

In `apply_demo_value`, parse:

```cpp
    if (key == "rsp_lane_select") {
        if (value == "target") {
            config->rsp_lane_select = TmRingRspLaneSelect::TARGET;
            return true;
        }
        if (value == "hash") {
            config->rsp_lane_select = TmRingRspLaneSelect::HASH;
            return true;
        }
        if (value == "round_robin") {
            config->rsp_lane_select = TmRingRspLaneSelect::ROUND_ROBIN;
            return true;
        }
        *error = "rsp_lane_select expects target, hash or round_robin, got: " + value;
        return false;
    }
```

Also route `rsp_phys_lanes` through `parse_u32`.

- [ ] **Step 4: Validate config**

In `validate_config`, include `config.rsp_phys_lanes == 0` in the non-zero check, and keep the error explicit:

```cpp
    if (config.rsp_phys_lanes == 0) {
        *error = "rsp_phys_lanes must be non-zero";
        return false;
    }
```

- [ ] **Step 5: Transfer demo config into ring config**

In `make_demo_ring_cfg` in `BUS/aicore/tm_ring_demo_test.h`, add:

```cpp
    cfg->rsp_phys_lanes = tc.rsp_phys_lanes;
    cfg->rsp_lane_select = tc.rsp_lane_select;
```

- [ ] **Step 6: Print new config fields**

In `TEST_BUS_CONFIG`, append:

```cpp
              << " rsp_phys_lanes=" << test_case.rsp_phys_lanes
              << " rsp_lane_select=" << rsp_lane_select_name(test_case)
```

Add helper:

```cpp
inline const char*
rsp_lane_select_name(const RingDemoConfig& tc)
{
    switch (tc.rsp_lane_select) {
        case TmRingRspLaneSelect::TARGET:
            return "target";
        case TmRingRspLaneSelect::HASH:
            return "hash";
        case TmRingRspLaneSelect::ROUND_ROBIN:
            return "round_robin";
        default:
            return "target";
    }
}
```

- [ ] **Step 7: Add TOML comments and defaults**

In `BUS/aicore/config/pem_config_cloud.toml`, under `[RING_DEMO]`, add:

```toml
# RSP 物理 lane 数。该参数才表示真实并行回包链路资源；
# rd_rsp_port_num 仅表示逻辑响应端口/通道编号，不直接增加物理带宽。
rsp_phys_lanes = 1

# RSP 物理 lane 选择策略：
# target      = 按 target/DDR channel 分配，适合模拟多 DRAM channel 回包；
# hash        = 按地址 hash 分配，适合观察 lane 均衡；
# round_robin = 轮询分配，适合压力测试，不建议作为默认架构假设。
rsp_lane_select = "target"
```

- [ ] **Step 8: Verify config-only behavior**

Run on Linux build environment:

```bash
cmake --build ../build
../build/exe/test_prj
grep -E 'TEST_(BUS_CONFIG|RESULT)' pem_multi_core_result.txt
```

Expected:

```text
TEST_BUS_CONFIG ... rsp_phys_lanes=1 rsp_lane_select=target ...
TEST_RESULT case=multi_core status=PASS
```

If the existing baseline currently fails for unrelated reasons, record the exact failure and continue only after confirming it is not caused by this task.

---

### Task 2: Carry physical RSP lane metadata in payload

**Files:**
- Modify: `BUS/aicore/tm/tm_pld.h`
- Modify: `BUS/aicore/tm/tm_pld.cc`

**Interfaces:**
- Produces:
  - `TmPld::ring_rsp_phys_lane`

- [ ] **Step 1: Add payload field**

In `BUS/aicore/tm/tm_pld.h`, near existing ring metadata:

```cpp
    uint32_t ring_rsp_phys_lane = 0;
```

Keep existing `ring_rsp_lane` unchanged. Its meaning remains logical RSP channel.

- [ ] **Step 2: Copy the field**

In `BUS/aicore/tm/tm_pld.cc`, copy the new field in the copy constructor:

```cpp
, ring_rsp_phys_lane(pld->ring_rsp_phys_lane)
```

- [ ] **Step 3: Verify compilation**

Run:

```bash
cmake --build ../build
```

Expected: build succeeds with no missing member or initializer errors.

---

### Task 3: Implement physical-lane selection at RSP injection

**Files:**
- Modify: `BUS/aicore/tm_ring_target_port.h`
- Modify: `BUS/aicore/tm_ring_target_port.cc`

**Interfaces:**
- Consumes:
  - `TmRingCfg::rsp_phys_lanes`
  - `TmRingCfg::rsp_lane_select`
  - `TmPld::ring_rsp_phys_lane`
- Produces:
  - All RSP subnet packets injected by target ports carry a stable physical lane.

- [ ] **Step 1: Add target-port lane-selection state**

In `BUS/aicore/tm_ring_target_port.h`, add private state:

```cpp
    uint32_t next_rsp_phys_lane_ = 0;
```

Add private helper declarations:

```cpp
    uint32_t rsp_phys_lane_count() const;
    uint32_t select_rsp_phys_lane(p_tm_pld_t rsp);
```

- [ ] **Step 2: Implement lane-count helper**

In `BUS/aicore/tm_ring_target_port.cc`:

```cpp
uint32_t TmRingTargetPort::rsp_phys_lane_count() const
{
    if (cfg_ == nullptr) {
        return 1;
    }
    return tm_ring_rsp_phys_lane_count(cfg_->rsp_phys_lanes);
}
```

- [ ] **Step 3: Implement lane selection**

In `BUS/aicore/tm_ring_target_port.cc`:

```cpp
uint32_t TmRingTargetPort::select_rsp_phys_lane(p_tm_pld_t rsp)
{
    const uint32_t lanes = rsp_phys_lane_count();
    if (lanes <= 1 || rsp == nullptr || cfg_ == nullptr) {
        return 0;
    }

    switch (cfg_->rsp_lane_select) {
        case TmRingRspLaneSelect::TARGET:
            return target_id_ % lanes;
        case TmRingRspLaneSelect::HASH:
            return static_cast<uint32_t>(
                ((rsp->addr >> 6) ^ (rsp->addr >> 12) ^ target_id_) % lanes);
        case TmRingRspLaneSelect::ROUND_ROBIN: {
            const uint32_t lane = next_rsp_phys_lane_ % lanes;
            next_rsp_phys_lane_ = (next_rsp_phys_lane_ + 1) % lanes;
            return lane;
        }
        default:
            return target_id_ % lanes;
    }
}
```

- [ ] **Step 4: Reset round-robin state**

In `TmRingTargetPort::reset`, set:

```cpp
    next_rsp_phys_lane_ = 0;
```

- [ ] **Step 5: Assign physical lane to all RSP-subnet packets**

In `recv_rd_cmd_rsp`, after existing fields:

```cpp
        rsp->ring_rsp_phys_lane = select_rsp_phys_lane(rsp);
```

In `recv_wr_cmd_rsp` and `recv_wr_dat_rsp`, after setting `ring_subnet`/`ring_traffic_class`, add:

```cpp
    rsp->ring_rsp_phys_lane = select_rsp_phys_lane(rsp);
```

Do not replace `rsp->ring_rsp_lane`; that field continues to select the logical output channel.

- [ ] **Step 6: Verify with single physical lane**

Run:

```bash
cmake --build ../build
../build/exe/test_prj
grep -E 'TEST_(COUNTS|RESULT)' pem_multi_core_result.txt
```

Expected: behavior remains equivalent to the previous `rsp_phys_lanes=1` run.

---

### Task 4: Make RSP link resources physically multi-lane

**Files:**
- Modify: `BUS/aicore/tm_ring_link.h`
- Modify: `BUS/aicore/tm_ring_link.cc`

**Interfaces:**
- Consumes:
  - `TmPld::ring_rsp_phys_lane`
  - `TmRingCfg::rsp_phys_lanes`
- Produces:
  - Independent RSP lane FIFO/inflight/serialization/bubble/stats.
  - Existing `subnet_stats(TmRingSubnet)` remains aggregate.
  - New `subnet_lane_stats(TmRingSubnet subnet, uint32_t lane)` exposes lane stats.

- [ ] **Step 1: Add lane helpers and data layout**

In `BUS/aicore/tm_ring_link.h`, replace per-subnet vectors with per-lane vectors:

```cpp
  uint32_t lane_count(TmRingSubnet subnet) const;
  uint32_t physical_lane(p_tm_pld_t pld) const;
  uint32_t lane_index(TmRingSubnet subnet, uint32_t lane) const;
  uint32_t total_lane_count() const;
```

Change data members to:

```cpp
  std::vector<uint32_t> inflight_count_;
  std::vector<tm_engine::tm_time_t> next_send_time_;
  std::vector<p_tm_com_que_t> inflight_packets_;
  std::vector<LinkSubnetStats> lane_stats_;
  std::vector<LinkSubnetStats> aggregate_stats_;
```

Add public accessor:

```cpp
  const LinkSubnetStats& subnet_lane_stats(TmRingSubnet subnet,
                                           uint32_t lane) const;
```

- [ ] **Step 2: Implement lane count**

In `BUS/aicore/tm_ring_link.cc`:

```cpp
uint32_t TmRingLink::lane_count(TmRingSubnet subnet) const
{
  if (subnet == TmRingSubnet::RSP && cfg_ != nullptr) {
    return tm_ring_rsp_phys_lane_count(cfg_->rsp_phys_lanes);
  }
  return 1;
}
```

REQ remains one physical lane.

- [ ] **Step 3: Implement lane index**

Use REQ lane 0 first, followed by RSP lanes:

```cpp
uint32_t TmRingLink::lane_index(TmRingSubnet subnet, uint32_t lane) const
{
  if (subnet == TmRingSubnet::REQ) {
    return 0;
  }
  return 1 + (lane % lane_count(TmRingSubnet::RSP));
}

uint32_t TmRingLink::total_lane_count() const
{
  return 1 + lane_count(TmRingSubnet::RSP);
}
```

- [ ] **Step 4: Implement packet physical-lane selection**

```cpp
uint32_t TmRingLink::physical_lane(p_tm_pld_t pld) const
{
  if (pld == nullptr) {
    return 0;
  }
  const auto subnet = static_cast<TmRingSubnet>(pld->ring_subnet);
  if (subnet != TmRingSubnet::RSP) {
    return 0;
  }
  return pld->ring_rsp_phys_lane % lane_count(TmRingSubnet::RSP);
}
```

- [ ] **Step 5: Allocate independent lane resources**

In `config`, allocate `total_lane_count()` queues and stats:

```cpp
  const uint32_t lanes = total_lane_count();
  next_send_time_.assign(lanes, 0);
  inflight_count_.assign(lanes, 0);
  lane_stats_.assign(lanes, LinkSubnetStats());
  aggregate_stats_.assign(tm_ring_subnet_count(), LinkSubnetStats());
```

Create queue names that include subnet/lane:

```cpp
  for (uint32_t i = 0; i < lanes; ++i) {
    inflight_packets_.push_back(tm_make_que<p_tm_pld_t>(
        clk_, name_ + "_lane" + std::to_string(i) + "_ready_packets",
        link_capacity_, latency_));
  }
```

- [ ] **Step 6: Update reset and idle**

Reset all lane vectors and all queues. `idle()` must check all physical lanes.

- [ ] **Step 7: Update can/accept path**

In `can_accept`, compute:

```cpp
  const auto subnet = static_cast<TmRingSubnet>(pld->ring_subnet);
  const uint32_t phys_lane = physical_lane(pld);
  const uint32_t idx = lane_index(subnet, phys_lane);
```

Use `idx` for `next_send_time_`, `inflight_count_`, `inflight_packets_`, and `lane_stats_`.

Keep aggregate stats consistent by adding rejected attempts to both lane and subnet aggregate. Use a helper if needed:

```cpp
void record_stall(uint32_t lane_idx, TmRingSubnet subnet,
                  uint64_t LinkSubnetStats::*field);
```

If a helper is used, define it privately in `tm_ring_link.h` and implement it in `tm_ring_link.cc`.

- [ ] **Step 8: Update reserve/enqueue/drain**

`reserve_send`, `enqueue_ready_packet`, and `drain_ready_packets` must use physical lane index, not only `pld->ring_subnet`.

Rules:

```text
REQ packet -> physical lane 0
RSP packet -> physical lane pld->ring_rsp_phys_lane
```

`next_send_time_` must be independent per physical lane, so two RSP packets on different lanes can serialize in the same cycle.

- [ ] **Step 9: Preserve aggregate stats API**

`subnet_stats(TmRingSubnet subnet)` must return aggregate stats. Do not remove callers.

Add:

```cpp
const TmRingLink::LinkSubnetStats&
TmRingLink::subnet_lane_stats(TmRingSubnet subnet, uint32_t lane) const
{
  return lane_stats_[lane_index(subnet, lane)];
}
```

- [ ] **Step 10: Verify physical lane parallelism**

Run with `rsp_phys_lanes=1` and `rsp_phys_lanes=4`.

Expected for `rsp_phys_lanes=4`:

```text
ring_link_serialization_busy_stalls should decrease for RSP-heavy traffic
TEST_RSP_LANE lines should show multiple active lanes when lane_select=target/hash/round_robin
```

---

### Task 5: Expose per-lane statistics and bottleneck visibility

**Files:**
- Modify: `BUS/aicore/tm_ring.h`
- Modify: `BUS/aicore/tm_ring_core.cc`
- Modify: `BUS/aicore/tm_ring_demo_test.h`

**Interfaces:**
- Produces:
  - `TmRingLinkLaneHotspot`
  - `TmRingFabric::ring_top_busy_lanes(TmRingSubnet subnet, uint32_t limit) const`
  - `TEST_RSP_LANE` result lines

- [ ] **Step 1: Add lane hotspot struct**

In `BUS/aicore/tm_ring.h`:

```cpp
struct TmRingLinkLaneHotspot
{
    uint32_t src_router = 0;
    TmRingPortDir src_dir = TmRingPortDir::LOCAL;
    uint32_t dst_router = 0;
    TmRingPortDir dst_dir = TmRingPortDir::LOCAL;
    TmRingSubnet subnet = TmRingSubnet::REQ;
    uint32_t phys_lane = 0;

    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t busy_cycles = 0;
    uint64_t serialization_busy_stall = 0;
    uint64_t inflight_limit_stall = 0;
    uint64_t link_fifo_full_stall = 0;
    uint64_t bubble_reserved_stall = 0;
    uint64_t downstream_fifo_full_stall = 0;
    uint64_t total_stalls = 0;
    uint32_t inflight_peak = 0;
};
```

Add API:

```cpp
std::vector<TmRingLinkLaneHotspot>
ring_top_busy_lanes(TmRingSubnet subnet, uint32_t limit) const;
```

- [ ] **Step 2: Implement lane hotspot query**

In `BUS/aicore/tm_ring_core.cc`, iterate links and all physical lanes for the requested subnet. For RSP, lane count is `cfg_->rsp_phys_lanes`; for REQ, lane count is 1.

Sort by `total_stalls`, then `busy_cycles`, descending.

- [ ] **Step 3: Keep existing aggregate breakdown**

In `ring_link_stall_breakdown`, continue using `subnet_stats(REQ)` and `subnet_stats(RSP)`. This preserves `TEST_BOTTLENECK`.

- [ ] **Step 4: Print RSP lane stats**

In `BUS/aicore/tm_ring_demo_test.h`, after `TEST_LINK_HOTSPOT`, add:

```cpp
auto rsp_lane_hotspots =
    ring->ring_top_busy_lanes(TmRingSubnet::RSP, 16);
for (const auto& lane : rsp_lane_hotspots) {
    std::cout << "TEST_RSP_LANE"
              << " src_router=" << lane.src_router
              << " dst_router=" << lane.dst_router
              << " phys_lane=" << lane.phys_lane
              << " packets=" << lane.packets
              << " bytes=" << lane.bytes
              << " busy_cycles=" << lane.busy_cycles
              << " serialization_busy_stalls="
              << lane.serialization_busy_stall
              << " inflight_limit_stalls="
              << lane.inflight_limit_stall
              << " fifo_full_stalls="
              << lane.link_fifo_full_stall
              << " bubble_stalls="
              << lane.bubble_reserved_stall
              << " downstream_fifo_full_stalls="
              << lane.downstream_fifo_full_stall
              << " total_stalls=" << lane.total_stalls
              << " inflight_peak=" << lane.inflight_peak
              << std::endl;
}
```

- [ ] **Step 5: Verify output**

Run:

```bash
../build/exe/test_prj
grep -E 'TEST_(BUS_CONFIG|BOTTLENECK|RSP_LANE|RESULT)' pem_multi_core_result.txt
```

Expected:

```text
TEST_BUS_CONFIG ... rsp_phys_lanes=...
TEST_BOTTLENECK ...
TEST_RSP_LANE ... phys_lane=...
TEST_RESULT case=multi_core status=PASS
```

---

### Task 6: Update read throughput peak calculation

**Files:**
- Modify: `BUS/aicore/tm_ring_demo_test.h`

**Interfaces:**
- Consumes:
  - `RingDemoConfig::rsp_phys_lanes`
- Produces:
  - Utilization calculation that reflects physical RSP bandwidth.

- [ ] **Step 1: Replace path-width calculation**

Current peak calculation uses:

```cpp
const uint32_t path_width =
    std::min(test_case.ring_link_width_bytes,
             test_case.target_width_bytes);
```

Replace with:

```cpp
const uint32_t rsp_path_width =
    std::min(test_case.ring_link_width_bytes * test_case.rsp_phys_lanes,
             test_case.target_width_bytes);
```

Then compute:

```cpp
const double estimated_peak_bpc =
    static_cast<double>(parallel_paths) * rsp_path_width;
```

- [ ] **Step 2: Print the effective RSP path width**

Append to `TEST_UTILIZATION`:

```cpp
              << " rsp_effective_path_width=" << rsp_path_width
```

- [ ] **Step 3: Verify utilization semantics**

Run two cases:

```bash
TM_RING_DEMO_OPTIONS="--rsp-phys-lanes 1" ../build/exe/test_prj
grep 'TEST_UTILIZATION' pem_multi_core_result.txt

TM_RING_DEMO_OPTIONS="--rsp-phys-lanes 4" ../build/exe/test_prj
grep 'TEST_UTILIZATION' pem_multi_core_result.txt
```

Expected:

```text
rsp_phys_lanes=4 increases estimated_peak_bytes_per_cycle versus rsp_phys_lanes=1
measurement_valid=yes for completed runs
```

Do not tune OSD or FIFO in this task unless the run fails to complete due to already-known backpressure limits.

---

### Task 7: Add focused regression scenarios

**Files:**
- Modify: `BUS/aicore/config/pem_config_cloud.toml`
- Modify: `BUS/aicore/tm_ring_demo_config.cc`

**Interfaces:**
- Produces:
  - Case `multi_core_rsp_4lane`

- [ ] **Step 1: Add TOML case**

In `BUS/aicore/config/pem_config_cloud.toml`:

```toml
[RING_DEMO.case.multi_core_rsp_4lane]
# 用于验证 RSP 多物理 lane 是否生效。该场景不改变拓扑，只增加 RSP link 并行资源。
rsp_phys_lanes = 4
rsp_lane_select = "target"
global_osd = 256
target_rd_osd = 4096
target_wr_osd = 4096
target_acc_osd = 4096
```

- [ ] **Step 2: Allow the new case name**

In `make_demo_case`, add:

```cpp
    if (name == "multi_core_rsp_4lane") {
        config.name = name;
        return config;
    }
```

- [ ] **Step 3: Verify both default and 4-lane case**

Run:

```bash
TM_RING_DEMO_CASE=multi_core ../build/exe/test_prj
grep -E 'TEST_(COUNTS|UTILIZATION|RSP_LANE|RESULT)' pem_multi_core_result.txt

TM_RING_DEMO_CASE=multi_core_rsp_4lane ../build/exe/test_prj
grep -E 'TEST_(COUNTS|UTILIZATION|RSP_LANE|RESULT)' pem_multi_core_result.txt
```

Expected:

```text
both cases complete with TEST_RESULT status=PASS
multi_core_rsp_4lane prints active TEST_RSP_LANE entries for more than one phys_lane
```

---

### Task 8: Final validation and review notes

**Files:**
- No new code files unless fixing issues found by validation.

**Interfaces:**
- Produces:
  - Verified build/test output.
  - Short review summary.

- [ ] **Step 1: Run formatting/sanity checks**

Run:

```bash
git diff --check
```

Expected:

```text
no trailing whitespace errors
```

- [ ] **Step 2: Run build**

Run:

```bash
cmake --build ../build
```

Expected: build succeeds.

- [ ] **Step 3: Run default multi-core test**

Run:

```bash
TM_RING_DEMO_CASE=multi_core ../build/exe/test_prj
grep -E 'TEST_(BUS_CONFIG|COUNTS|UTILIZATION|BOTTLENECK|RSP_LANE|RESULT)' pem_multi_core_result.txt
```

Expected:

```text
TEST_RESULT case=multi_core status=PASS
```

- [ ] **Step 4: Run 4-lane RSP test**

Run:

```bash
TM_RING_DEMO_CASE=multi_core_rsp_4lane ../build/exe/test_prj
grep -E 'TEST_(BUS_CONFIG|COUNTS|UTILIZATION|BOTTLENECK|RSP_LANE|RESULT)' pem_multi_core_result.txt
```

Expected:

```text
TEST_BUS_CONFIG ... rsp_phys_lanes=4 rsp_lane_select=target ...
TEST_RESULT case=multi_core_rsp_4lane status=PASS
TEST_RSP_LANE appears for multiple phys_lane values
```

- [ ] **Step 5: Write review summary**

Include these points in the handoff:

```text
1. rd_rsp_port_num remains logical; rsp_phys_lanes controls physical RSP bandwidth.
2. Router is shared; ring/network is not duplicated.
3. REQ remains single physical lane.
4. RSP lanes have independent serializer, FIFO, inflight, bubble and stall stats.
5. New TEST_RSP_LANE output confirms whether traffic is actually distributed.
```

---

## Self-Review

**Spec coverage:**  
This plan covers the requested architecture choice: shared router, RSP multi physical lanes, no full subnet/network duplication, no extra REQ physical split in the first pass.

**Placeholder scan:**  
No `TBD`, `TODO`, or unspecified "handle later" steps are intentionally left.

**Type consistency:**  
The plan consistently uses `rsp_phys_lanes`, `rsp_lane_select`, `ring_rsp_phys_lane`, `TmRingRspLaneSelect`, and leaves existing `ring_rsp_lane` / `rd_rsp_port_num` as logical-channel concepts.

## Execution Handoff

Plan complete. Recommended execution mode:

1. Subagent-Driven if available: one fresh implementer per task with review between tasks.
2. Inline Execution if working in this same session: execute tasks sequentially, run validation after each task.

Do not start implementation until the owner confirms this plan matches the intended modeling semantics.
