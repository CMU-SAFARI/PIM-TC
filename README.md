# Accelerating Triangle Counting with Real Processing-in-Memory Systems

This repository contains the source code for **PIM-TC**, an implementation of the Triangle Counting (TC) algorithm optimized for the UPMEM Processing-in-Memory (PIM) architecture.

## Overview

Triangle Counting (TC) is a fundamental graph analysis task used in various domains. However, TC is memory-bound and often struggles to scale on conventional CPU/GPU systems due to high memory bandwidth requirements and low data reuse. Processing-in-Memory (PIM) offers a promising solution by placing computation closer to memory.

This work presents the first TC algorithm designed for and evaluated on the real-world UPMEM PIM system. Our implementation addresses the specific limitations of this architecture:

1.  **Expensive Inter-Core Communication:** We employ a **vertex coloring** technique to partition the graph edges among the PIM cores such that triangles can be counted locally without the need for communication between PIM cores.
2.  **Limited DPU Memory:** We use **reservoir sampling** within each PIM core's local DRAM bank (MRAM) to handle graphs larger than the available memory, providing an approximate count with statistical adjustments.
3.  **Host-PIM Data Transfer Overhead:** **Uniform sampling** at the host level reduces the number of edges transferred to the PIM cores, speeding up data preparation and execution for approximate results.
4.  **High-Degree Nodes:** We optionally use the **Misra-Gries** summary algorithm during host processing to identify and re-label high-degree nodes, optimizing the edge-iterator approach on the PIM cores.

Our implementation accepts graphs in the standard Coordinate List (COO) format and demonstrates significant speedups compared to CPU-based methods for dynamic graph workloads where the COO format allows for efficient updates. Refer to the [Dynamic Graph TC Branch](https://github.com/CMU-SAFARI/PIM-TC/tree/dynamic-graph-TC) for an implementation specifically designed to support dynamic graph updates.

![Edges partitioning](img/edges_partitioning.png)
_Partitioning of the graph’s edges among the PIM cores through the host processor_

## Repository Structure

```
.
├── LICENSE             # Project license information
├── Makefile            # Build configuration for the project
├── README.md           # This file
├── common/             # Common headers used by host and DPU
│   └── common.h
├── dpu/                # Source code to be run on UPMEM DPUs
│   ├── dpu_util.c
│   ├── dpu_util.h
│   ├── locate_nodes.c
│   ├── locate_nodes.h
│   ├── quicksort.c
│   ├── quicksort.h
│   ├── task.c
│   ├── triangle_counter.c
│   └── triangle_counter.h
├── host/               # Source code running on the host CPU
│   ├── app.c
│   ├── handle_edges_parallel.c
│   ├── handle_edges_parallel.h
│   ├── host_util.c
│   ├── host_util.h
│   ├── mg_hashtable.c
│   └── mg_hashtable.h
└── img/                # Images used in README
    └── edges_partitioning.png
```

## Prerequisites

-   **UPMEM SDK:** The code was tested with `UPMEM SDK v2024.1.0`. Download from the [UPMEM website](https://sdk.upmem.com/). Ensure the SDK environment is activated (`source <sdk_path>/upmem_env.sh`).
-   **Build Tools:** A standard C compiler (like `gcc`) and `make`.

## Installation

Compile the host and DPU code using the provided Makefile:

```bash
make
```

This will create the executable `app` in the `bin/` directory.

## Usage

### Configuration

Before compiling, you can adjust the following parameters:

-   **`NR_TASKLETS`** (in [Makefile](Makefile)): Number of tasklets (software threads) per DPU. Must be a power of 2. The paper uses 16 for best performance.
-   **`NR_DPUS`** (in [Makefile](Makefile)): Number of DPUs to use. This is determined by the number of colors (`C`) chosen for vertex coloring via the `-c` flag (see below). The Makefile value should match the value derived from C using the formula: `NR_DPUS = Binomial(C + 2, 3)`. Ensure the target system has at least this many DPUs available.
-   **`NR_THREADS`** (in [Makefile](Makefile)): Number of CPU threads used by the host for parallel processing (e.g., reading the graph, preparing batches). Set to the number of available CPU cores/threads for best host performance (the paper uses **32**).
-   **`WRAM_BUFFER_SIZE`** (in [`dpu_util.h`](dpu/dpu_util.h)): Size of the buffer in the DPU's scratchpad memory (WRAM) for holding edges during processing. Must not exceed 2048 bytes.

### Running the Application

Navigate to the `bin` directory after compilation and execute the application:

```bash
./app -c <nr_colors> -f <path_to_graph_file> [OPTIONS]
```

**Required Arguments:**

-   `-c <nr_colors>`: **(Required)** Number of colors (`C`) to use for vertex coloring. This determines the number of DPUs allocated (`Binomial(C + 2, 3)`).

-   `-f <path_to_graph_file>`: **(Required)** Path to the input graph file in Coordinate List (COO) (Matrix Market (.mtx)) format.

**Optional Arguments (for approximation and optimization):**

-   `-p <keep_percentage>`: Probability (0.0 to 1.0) of keeping an edge during **uniform sampling** at the host level. Default is `1.0` (no sampling). Values less than 1.0 enable approximation for faster processing.

-   `-M <sample_size>`: Maximum number of edges to store in each DPU's MRAM bank for **reservoir sampling**. If the number of edges assigned to a DPU exceeds this, sampling occurs. Defaults to the maximum possible based on available MRAM if not specified. Enables approximation if triggered.

-   `-k <Misra_Gries_dictionary_size>`: Size (`K`) of the dictionary used by the **Misra-Gries** algorithm on the host (per thread) to find frequent (high-degree) nodes. Enables the high-degree node optimization if set.

-   `-t <nr_most_frequent_nodes_sent>`: Number (`t`) of the most frequent nodes (identified by Misra-Gries) whose edges are remapped before sending to DPUs. Default is 5. Ignored if `-k` is not set.

-   `-s <seed>`: Seed for random number generation used in coloring and sampling. Uses a random seed if not specified.

**Example:**

```bash
# Run exact count using 10 colors (-> 220 DPUs) on graph.mtx
./app -c 10 -f ../data/graph.mtx

# Run approximate count using 8 colors (-> 120 DPUs), keeping 10% of edges (p=0.1)
./app -c 8 -p 0.1 -f ../data/graph.mtx

# Run exact count using 12 colors (-> 364 DPUs) with Misra-Gries (K=1000, t=20)
./app -c 12 -k 1000 -t 20 -f ../data/graph.mtx
```

## Reproducibility

This section provides details on the datasets and procedures used in the experiments described in the accompanying paper to facilitate reproducibility.

### Dataset Preprocessing

The input graphs must be preprocessed before being used. The preprocessing involves:

1.  Reading the raw graph file (assuming COO format, e.g., space-separated node ID pairs per line).
2.  Removing self-loops (edges `(u, u)`).
3.  Removing duplicate edges (treating `(u, v)` and `(v, u)` as the same edge).
4.  Shuffling the resulting unique edges randomly.
5.  Prepending a header line to make the format Matrix Market (`max_node_id max_node_id num_edges`).

A simple Python script can perform steps 1-3. Using an equivalent implementation in C++ might offer better performance for very large graphs.

**Python Preprocessing Function:**

```python
def preprocess_graph(input_filename, output_filename):
    seen_edges = set()
    max_node_id = -1
    valid_edge_count = 0

    with open(input_filename, 'r') as infile, open(output_filename, 'w') as outfile:
        for line_num, line in enumerate(infile):

            edge_nodes = line.split()
            u, v = map(int, edge_nodes)

            max_node_id = max(max_node_id, u, v)

            # Remove self-loops
            if u == v:
                continue

            # Store edges uniquely
            edge = tuple(sorted((u, v)))

            # Add to set and write if it's a new edge
            if edge not in seen_edges:
                seen_edges.add(edge)
                outfile.write(f"{u} {v}\n") # Write original u v as in COO
                valid_edge_count += 1

    print(f"Finished processing.")
    print(f"Max Node ID found: {max_node_id}")
    print(f"Number of unique, non-self-loop edges: {valid_edge_count}")
    return max_node_id, valid_edge_count
```

**Post-processing Steps:**

1.  **Shuffle the cleaned graph:**
    ```bash
    shuf cleaned_graph.txt > shuffled_graph.txt
    ```
2.  **Prepend header for Matrix Market format:** Use `max_node_id` and `valid_edge_count` from the preprocessing step and create the final input file (`final_graph.mtx`). The header line must be `max_node_id max_node_id valid_edge_count`.

### Datasets Used in Evaluation

The following table details the graphs used, including their sources and properties after preprocessing.

| Graph Name    | Source                                                                              | Valid Edges | Unique Nodes | Max Node ID | Max Degree | Average Degree | Triangles      | Connected Triples  | Global Clustering Coefficient |
| :------------ | :---------------------------------------------------------------------------------- | :---------- | :----------- | :---------- | :--------- | :------------- | :------------- | :----------------- | :---------------------------- |
| Kronecker 23  | [Graph500 Generator](#graph500-kronecker-generator) (`N=23, -e 16`)                 | 129,335,985 | 4,609,311    | 8,388,607   | 257,484    | 56.12          | 4,675,811,428  | 672,277,292,688    | 0.0209                        |
| Kronecker 24  | [Graph500 Generator](#graph500-kronecker-generator) (`N=24, -e 16`)                 | 260,383,358 | 8,870,393    | 16,777,214  | 407,017    | 58.71          | 10,285,674,980 | 1,783,231,368,677  | 0.0173                        |
| V1r           | [SuiteSparse (GenBank)](https://sparse.tamu.edu/GenBank/kmer_V1r)                   | 232,705,452 | 214,005,017  | 214,005,018 | 8          | 2.17           | 49             | 307,294,717        | 4.784e-7                      |
| LiveJournal   | [SNAP](https://snap.stanford.edu/data/soc-LiveJournal1.html)                        | 42,851,237  | 4,847,571    | 4,847,571   | 20,333     | 17.68          | 285,730,264    | 7,269,503,753      | 0.1179                        |
| Orkut         | [SNAP](https://snap.stanford.edu/data/com-Orkut.html)                               | 117,185,083 | 3,072,441    | 3,072,627   | 33,313     | 76.28          | 627,584,181    | 45,625,466,571     | 0.0413                        |
| Human-Jung    | [Network Repository](https://networkrepository.com/bn-human-Jung2015-M87113878.php) | 267,844,669 | 784,262      | 1,822,418   | 21,743     | 683.05         | 41,727,013,307 | 425,248,603,910    | 0.2944                        |
| WikipediaEdit | [Konect](http://konect.cc/networks/edit-enwiki/)                                    | 255,688,945 | 42,541,517   | 42,640,546  | 3,026,864  | 12.02          | 881,439,081    | 33,785,432,110,811 | 7.827e-5                      |

**Graph500 Kronecker Generator:**

-   **Repository:** [https://github.com/RapidsAtHKUST/Graph500KroneckerGraphGenerator](https://github.com/RapidsAtHKUST/Graph500KroneckerGraphGenerator)
-   **Specification:** [https://graph500.org/?page_id=12](https://graph500.org/?page_id=12)
-   **Generation Command:** `./generator_omp N -e 16 -o output.txt`
    -   `N`: Scale factor.
    -   `-e 16`: Edge factor.
    -   The output file requires preprocessing as described above.

### Running Experiments

**General Methodology:** For each configuration tested in the paper, **2 warmup runs** were performed, followed by **5 measurement runs**. The average time of the 5 measurement runs is reported in the paper. Ensure the PIM application is compiled and the datasets are preprocessed. Run PIM commands from the `bin/` directory.

**1. PIM DPU Scaling:**

-   Modify `NR_DPUS` in the `Makefile` to match the number of DPUs corresponding to the chosen color count `C` (using `NR_DPUS = Binomial(C + 2, 3)`). Recompile (`make clean && make`).
-   Run for various `C` values tested in the paper on all graphs.

```bash
# Example for C=20 (1540 DPUs - adjust Makefile first)
./app -c 20 -f ../data/final_graph.mtx
```

**2. PIM Misra-Gries Summary:**

-   Use a fixed number of colors `C=23` (corresponding to 2300 DPUs - set `NR_DPUS=2300` in Makefile and compile).
-   Iterate through `K` in `{0, 2500, 5000, 10000}` and `T` in `{0, 10, 50, 100, 250}`. Note that `K=0` disables Misra-Gries.

```bash
# Example run
./app -c 23 -k 5000 -t 50 -f ../data/final_graph.mtx
```

**3. PIM Uniform Sampling:**

-   Use `C=23` (NR_DPUS=2300).
-   Use the best performing K and T parameters found from the Misra-Gries tests for each graph:

|     Graph     | Best K | Best T |
| :-----------: | :----: | :----: |
| Kronecker 23  | 10000  |  250   |
| Kronecker 24  |  5000  |  250   |
|      V1r      |   0    |   0    |
|  LiveJournal  |   0    |   0    |
|     Orkut     |   0    |   0    |
|  Human-Jung   |   0    |   0    |
| WikipediaEdit |  2500  |  250   |

-   Iterate through `p` in `{0.5, 0.25, 0.1, 0.01}`.

```bash
# Example run for Kronecker 23 with p=0.1
./app -c 23 -p 0.1 -k 10000 -t 250 -f ../data/kronecker_23.mtx

# Example run for LiveJournal with p=0.5 (no Misra-Gries needed)
./app -c 23 -p 0.5 -k 0 -t 0 -f ../data/liveJournal.mtx
```

**4. PIM Reservoir Sampling:**

-   Use `C=23` (NR_DPUS=2300).
-   Use the best performing `K` and `T` parameters (same as above).
-   Calculate the maximum expected number of edges assigned to each DPU. Using the estimate for the most loaded DPUs (`6N`): `max_sample_size_approx = (6.0 / (C**2)) * total_edges`.
-   Iterate through `p` in `{0.5, 0.25, 0.1, 0.01}`.
-   Set `SAMPLE_SIZE = round(p * max_sample_size_approx)`. Use `-M SAMPLE_SIZE` option with the `final_graph.mtx` file.

```bash
# Example calculation for Kronecker 23 (129,335,985 edges)
# C = 23
# total_edges = 129335985
# max_sample_size_approx = (6.0 / (23*23)) * 129335985 ≈ 1466694
# For p = 0.1, SAMPLE_SIZE = round(0.1 * 1466694) = 146669

# Example run for Kronecker 23 with p=0.1 for reservoir size
./app -c 23 -M 146669 -k 10000 -t 250 -f ../data/kronecker_23.mtx
```

**5. CPU/GPU Comparisons (static graphs):**

-   **CPU Baseline (Bader-Research):**

    -   Clone the repository: `git clone https://github.com/Bader-Research/triangle-counting.git`
    -   Modify `src/main.c`: Comment out all `benchmarkTC_P` or `benchmarkTC` lines except the one for `tc_MapJIK_P`.
    -   Compile the code according to the repository's instructions (e.g., using `make`). Let the compiled executable be `tc_cpu`.
    -   Run the command:
        ```bash
        ./tc_cpu -P -f ../data/graph.mtx -x -q
        ```
    -   The reported time for `tc_MapJIK_P` is the triangle counting time used for comparison. Note that the internal COO to CSR conversion time performed by this code is _excluded_ from the comparison results.

-   **GPU Baseline (cuGraph):**

    -   Use the preprocessed graph file **without** the header.
    -   Ensure `cugraph` and `cudf` are installed in your Python environment.
    -   Use the following Python function:

        ```python
        import cugraph
        import cudf
        import time

        def run_cugraph_tc(file_path):
            start_load = time.time()
            # Read graph from COO (space separated, no header)
            gdf = cudf.read_csv(filepath_or_buffer=file_path,
                                delimiter=' ',
                                dtype=['int32', 'int32'],
                                header=None,
                                names=['src', 'dst'])

            G = cugraph.Graph(directed=False)
            G.from_cudf_edgelist(gdf, source='src', destination='dst')

            load_graph_time = time.time() - start_load
            print(f"Graph Loading Time: {load_graph_time:.4f} s")

            start_count = time.time()

            count = cugraph.triangle_count(G)

            count_time = time.time() - start_count
            print(f"Triangle Count Time: {count_time:.4f} s")

            return count / 3
        ```

**6. CPU/GPU Comparisons (dynamic graphs):**

Refer to the [Dynamic Graph TC Branch](https://github.com/CMU-SAFARI/PIM-TC/tree/dynamic-graph-TC).

## Citation

Please use the following citations to cite this work, if you find this repository useful:

Lorenzo Asquini, Manos Frouzakis, Juan Gómez-Luna, Mohammad Sadrosadati, Onur Mutlu, Francesco Silvestri, "[Accelerating Triangle Counting with Real Processing-in-Memory Systems](https://arxiv.org/abs/2505.04269)", arXiv:2505.04269 [cs.AR], 2025.

Bibtex entry for citation:

```
@misc{asquini2025accelerating,
    title={Accelerating Triangle Counting with Real Processing-in-Memory Systems},
    author={Lorenzo Asquini and Manos Frouzakis and Juan Gómez-Luna and Mohammad Sadrosadati and Onur Mutlu and Francesco Silvestri},
    year={2025},
    eprint={2505.04269},
    archivePrefix={arXiv},
    primaryClass={cs.AR}
}
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgements

This work was supported in part by the Big-Mobility project (Uni-Impresa call, University of Padova), MUR PRIN 20174LF3T8 AHEAD, MUR PNRR CN00000013 (HPC, Big Data, Quantum Computing), ETH Future Computing Laboratory (EFCL), Huawei ZRC Storage Team, Semiconductor Research Corporation, AI Chip Center for Emerging Smart Systems (ACCESS), InnoHK funding, Hong Kong SAR, and European Union's Horizon programme [101047160 - BioPIM]. We acknowledge generous gifts from Google, Huawei, Intel, and Microsoft.
