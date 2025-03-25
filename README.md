# Triangle Counting (TC) on the UPMEM PIM Architecture

This repository contains code for counting triangles in graphs using the UPMEM Processing-In-Memory architecture. The input graphs are provided in Coordinate (COO) format, and the implementation is optimized for efficient execution on DPUs. Refer to the [Dynamic Graph TC Branch](https://github.com/CMU-SAFARI/PIM-TC/tree/dynamic-graph-TC) for an implementation that supports dynamic updates to the input graph.

The code was tested using the [UPMEM SDK version 2024.1.0](https://sdk.upmem.com/) on both real hardware and the provided functional model.

## Modifying the [Makefile](Makefile)

-   Adjust the number of tasklets per DPU by modifying `NR_TASKLETS`. This value must be a power of 2. The best-performing configuration uses 16 tasklets.
-   Set the number of DPUs by modifying `NR_DPUS`. This should be computed based on the number of colors ($C$) using the formula:
    ```
    NR_DPUS = Binom(C+2, 3)
    ```
-   Change the number of threads used by the host processor via `NR_THREADS`. The optimal setting matches the number of available CPU threads.

## Running the Code

After compiling (`make`), navigate to the `bin` directory and execute:

```
./app -s seed -M sample_size -p keep_percentage -k Misra_Gries_dictionary_size -t nr_most_frequent_nodes_sent -c nr_colors -f path_to_graph_file
```

### Parameters:

-   `-s seed`: Seed for random number generation (random if not specified).
-   `-M sample_size`: Sample size inside the DPUs (defaults to max allowed if not given).
-   `-p keep_percentage`: Probability of keeping an edge (default: 1, meaning no edges are ignored).
-   `-k Misra_Gries_dictionary_size`: Max dictionary size for Misra-Gries per thread (ignored if not set).
-   `-t nr_most_frequent_nodes_sent`: Number of top frequent nodes sent to the DPUs (ignored if Misra-Gries is disabled, default: 5).
-   `-c nr_colors` (**Required**): Number of colors used for graph coloring, also determining the number of DPUs.
-   `-f path_to_graph_file` (**Required**): Path to the graph file in COO (Matrix Market) format.

## Other Modifications

-   The WRAM buffer size can be adjusted in [`dpu_util.h`](dpu/dpu_util.h) by modifying `WRAM_BUFFER_SIZE`. Do not exceed 2048 bytes.
