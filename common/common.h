#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

// Struct used to transfer starting arguments from the host to the DPU. Aligned to 8 bytes
typedef struct {
	uint32_t seed;
	uint32_t sample_size;
	uint32_t t;
	uint32_t padding;
} dpu_arguments_t;

typedef struct {
	uint32_t execution_code;
	uint32_t max_node_id;
} execution_config_t;

// Contains the two nodes that make an edge
typedef struct {
	uint32_t u;
	uint32_t v;
} edge_t;

// Contains a pair of colors, representing the colors of an edge
typedef struct {
	uint32_t color_u;
	uint32_t color_v;
} edge_colors_t;

// Used to store information about nodes while using Misra-Gries
typedef struct {
	uint32_t node_id;
	int32_t  frequency;
} node_frequency_t;

#endif /* __COMMON_H__ */
