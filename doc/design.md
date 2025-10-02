# Analytical Model Design

## Overview
TBD


## Design Principles
TBD



## Micro Architecture
- **PE**    - Processing Element
- **PE Array** - A grid of PEs that work in parallel to process data. PEs in same column can accumulate results.
- **Global Memory** - Shared memory accessible by all PEs, used for storing data that needs to be processed.
- **Local Memory** - Memory local to each PE, used for storing intermediate results and data that is frequently accessed by the PE.
- **Interconnect** - The network that connects PEs and allows them to communicate with each other and access global memory.
- **Control Unit** - Manages the operation of the PE array, including scheduling tasks and managing data flow.
- **Non-linear Function Unit** - A specialized unit within the PE that performs non-linear operations, such as activation functions in neural networks.

## Current Design
- **PE Array**: row = 7, column = 4, 6 clusters, total 168 PEs
- **Global Memory**: 64KB, 4 banks
- **Local Memory**: 512B per PE
