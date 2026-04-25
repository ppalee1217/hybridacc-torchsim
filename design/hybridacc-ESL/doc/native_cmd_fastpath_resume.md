# Native Cmd + CmdFabric Fast Path Resume Log

## Goal

- Replace the active CoreController to ComputeCluster command path with the native command interface.
- Follow up with CmdFabric fast path so core-visible cluster MMIO avoids the extra relay cycle.
- Keep existing ComputeCluster AHB slave support alive only for standalone cluster tests during migration.

## Working Rules

- Coding style follows the simulator coding convention in `doc/component/coding_convention.md`.
- If Python is needed later, use `uv`.
- Validate with the narrowest available ESL test target before widening scope.

## Phase Status

| Phase | Status | Notes |
| --- | --- | --- |
| Phase 0 baseline / guardrails | Completed | Focused ESL validation anchor is now `test_cluster_unit` plus `test_core_controller_integration`. |
| Phase 1 native interface plumbing | Completed | Added native cluster command handshake across CoreController, HybridAcc, CmdFabric, and ComputeCluster. |
| Phase 2 semantic extraction | Completed | ComputeCluster MMIO read/write semantics now live in shared helpers used by both AHB and native frontends. |
| Phase 3 fast cluster frontend | Completed | Native request path is active in ComputeCluster; write ack is 1 cycle and read response is 2 cycles at the cluster boundary. |
| Phase 4 CmdFabric switch | Completed | HybridAcc active path no longer instantiates or uses CmdToAhbBridge. |
| Phase 5 CmdFabric fast path | Completed | Cluster unicast uses a direct CmdFabric fast path while broadcast and non-cluster targets remain on the slow FSM. |
| Phase 6 legacy cleanup | Completed with scope limit | Active bridge path is removed from HybridAcc. ComputeCluster AHB slave is intentionally retained for standalone cluster tests. |

## Current Design Decisions

- `ComputeCluster` keeps its AHB slave port for existing standalone tests, but HybridAcc runtime traffic now uses the native command frontend.
- The native frontend remains single-outstanding per cluster.
- CmdFabric fast path is limited to unicast cluster windows. Broadcast writes and all non-cluster targets still use the existing slow FSM.

## Validation Plan

- Build `design/hybridacc-ESL/test/build` if missing.
- Compile checks:
	- `cmake --build . --target test_core_controller_integration test_cluster_unit -j$(nproc)`
	- `cmake --build . --target test_cluster_sim test_cluster_sim_advanced -j$(nproc)`
- Behavioral check:
	- `ctest --output-on-failure -R 'ComputeCluster_Unit_Tests|HACC_Core_Controller_Integration'`

## Touched Files

- `simulator/include/Core/CoreController.hpp`
- `simulator/include/Core/CmdFabric.hpp`
- `simulator/include/HybridAcc.hpp`
- `simulator/include/ComputeCluster.hpp`
- `test/test_cluster_unit.cpp`
- `test/test_cluster_sim.cpp`
- `test/test_cluster_sim_advanced.cpp`
- `test/test_cluster_sim_backup.cpp`

## Change Log

- Initial work log created.
- Switched the active HybridAcc cluster command path from `CmdToAhbBridge` to the native command frontend.
- Added CmdFabric fast unicast path with direct response relay back to the core MMIO interface.
- Kept ComputeCluster AHB MMIO support for standalone tests, but made AHB and native paths share the same MMIO semantic helpers.
- Updated existing cluster testbenches to bind the new native command ports with dummy signals.
- Validation passed:
	- `test_cluster_unit`
	- `test_core_controller_integration`
	- `ctest -R 'ComputeCluster_Unit_Tests|HACC_Core_Controller_Integration'`
	- `test_cluster_sim`
	- `test_cluster_sim_advanced`