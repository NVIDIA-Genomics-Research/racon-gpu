#include <algorithm>
#include <cstring>

#include "cudapoa_batch.hpp"
#include "cudapoa_kernels.cuh"

#ifndef TABS
#define TABS printTabs(bid_)
#endif

inline std::string printTabs(uint32_t tab_count)
{
    std::string s;
    for(uint32_t i = 0; i < tab_count; i++)
    {
        s += "\t";
    }
    return s;
}

namespace nvidia {

namespace cudapoa {

uint32_t Batch::batches = 0;

Batch::Batch(uint32_t max_poas, uint32_t max_sequences_per_poa)
    : max_poas_(max_poas)
    , max_sequences_per_poa_(max_sequences_per_poa)
{
    bid_ = Batch::batches++;

    // Allocate host memory and CUDA memory based on max sequence and target counts.

    // Verify that maximum sequence size is in multiples of tb size.
    // We subtract one because the matrix dimension needs to be one element larger
    // than the sequence size.
    if (CUDAPOA_MAX_SEQUENCE_SIZE % NUM_THREADS != 0)
    {
        std::cerr << "Thread block size needs to be in multiples of 32." << std::endl;
        exit(-1);
    }

    uint32_t input_size = max_poas_ * max_sequences_per_poa * CUDAPOA_MAX_SEQUENCE_SIZE; //TODO how big does this need to be

    // Input buffers.
    CU_CHECK_ERR(cudaHostAlloc((void**) &inputs_h_, input_size * sizeof(uint8_t),
                  cudaHostAllocDefault));
    CU_CHECK_ERR(cudaHostAlloc((void**) &num_sequences_per_window_h_, max_poas_ * sizeof(uint16_t),
            cudaHostAllocDefault));
    CU_CHECK_ERR(cudaHostAlloc((void**) &sequence_lengths_h_, max_poas_ * max_sequences_per_poa_ * sizeof(uint16_t),
            cudaHostAllocDefault));
    CU_CHECK_ERR(cudaHostAlloc((void**) &window_details_h_, max_poas_ * sizeof(WindowDetails),
            cudaHostAllocDefault));

    //device allocations
    CU_CHECK_ERR(cudaMalloc((void**)&inputs_d_, input_size * sizeof(uint8_t)));
    CU_CHECK_ERR(cudaMalloc((void**)&sequence_lengths_d_, max_poas_ * max_sequences_per_poa * sizeof(uint16_t)));
    CU_CHECK_ERR(cudaMalloc((void**)&window_details_d_, max_poas_ * sizeof(nvidia::cudapoa::WindowDetails)));

    std::cerr << TABS << bid_ << " Allocated input buffers of size " << (static_cast<float>(input_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Output buffers.
    input_size = max_poas_ * CUDAPOA_MAX_SEQUENCE_SIZE;
    CU_CHECK_ERR(cudaHostAlloc((void**) &consensus_h_, input_size * sizeof(uint8_t),
                  cudaHostAllocDefault));

    CU_CHECK_ERR(cudaMallocPitch((void**) &consensus_d_,
                    &consensus_pitch_,
                    sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW,
                    max_poas_));
    std::cerr << TABS << bid_ << " Allocated output buffers of size " << (static_cast<float>(input_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Buffers for storing NW scores and backtrace.
    CU_CHECK_ERR(cudaMalloc((void**) &scores_d_, sizeof(int16_t) * CUDAPOA_MAX_MATRIX_GRAPH_DIMENSION * CUDAPOA_MAX_MATRIX_SEQUENCE_DIMENSION * max_poas_));
    CU_CHECK_ERR(cudaMalloc((void**) &alignment_graph_d_, sizeof(int16_t) * CUDAPOA_MAX_MATRIX_GRAPH_DIMENSION * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &alignment_read_d_, sizeof(int16_t) * CUDAPOA_MAX_MATRIX_GRAPH_DIMENSION * max_poas_ ));

    // Debug print for size allocated.
    uint32_t temp_size = (sizeof(int16_t) * CUDAPOA_MAX_MATRIX_GRAPH_DIMENSION * CUDAPOA_MAX_MATRIX_SEQUENCE_DIMENSION * max_poas_ );
    temp_size += 2 * (sizeof(int16_t) * CUDAPOA_MAX_MATRIX_GRAPH_DIMENSION * max_poas_ );
    std::cerr << TABS << bid_ << " Allocated temp buffers of size " << (static_cast<float>(temp_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Allocate graph buffers. Size is based maximum data per window, times number of windows being processed.
    CU_CHECK_ERR(cudaMalloc((void**) &nodes_d_, sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &node_alignments_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &node_alignment_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &incoming_edges_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &incoming_edge_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &outgoing_edges_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &outgoing_edge_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &incoming_edges_weights_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &outoing_edges_weights_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &sorted_poa_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &sorted_poa_node_map_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &sorted_poa_local_edge_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &consensus_scores_d_, sizeof(int32_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &consensus_predecessors_d_, sizeof(int16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &node_marks_d_, sizeof(int8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &check_aligned_nodes_d_, sizeof(bool) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMalloc((void**) &nodes_to_visit_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));

    CU_CHECK_ERR(cudaMemset(nodes_d_, 0, sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(node_alignments_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(node_alignment_count_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(incoming_edges_d_,0,  sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(incoming_edge_count_d_,0,  sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(outgoing_edges_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(outgoing_edge_count_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(incoming_edges_weights_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(outoing_edges_weights_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(sorted_poa_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(sorted_poa_local_edge_count_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(consensus_scores_d_, -1, sizeof(int32_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(consensus_predecessors_d_, -1, sizeof(int16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(node_marks_d_, 0, sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(check_aligned_nodes_d_, 0, sizeof(bool) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));
    CU_CHECK_ERR(cudaMemset(nodes_to_visit_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ));

    // Debug print for size allocated.
    temp_size = sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(int16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(int16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(int8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(bool) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * max_poas_ ;
    std::cerr << TABS << bid_ << " Allocated temp buffers of size " << (static_cast<float>(temp_size)  / (1024 * 1024)) << "MB" << std::endl;
}

Batch::~Batch()
{
    // Free all the host and CUDA memory.
    CU_CHECK_ERR(cudaFree(consensus_d_));

    CU_CHECK_ERR(cudaFree(scores_d_));
    CU_CHECK_ERR(cudaFree(alignment_graph_d_));
    CU_CHECK_ERR(cudaFree(alignment_read_d_));

    std::cerr << TABS << "Destroyed buffers." << std::endl;

    CU_CHECK_ERR(cudaFree(nodes_d_));
    CU_CHECK_ERR(cudaFree(node_alignments_d_));
    CU_CHECK_ERR(cudaFree(node_alignment_count_d_));
    CU_CHECK_ERR(cudaFree(incoming_edges_d_));
    CU_CHECK_ERR(cudaFree(incoming_edge_count_d_));
    CU_CHECK_ERR(cudaFree(outgoing_edges_d_));
    CU_CHECK_ERR(cudaFree(outgoing_edge_count_d_));
    CU_CHECK_ERR(cudaFree(incoming_edges_weights_d_));
    CU_CHECK_ERR(cudaFree(outoing_edges_weights_d_));
    CU_CHECK_ERR(cudaFree(sorted_poa_d_));
    CU_CHECK_ERR(cudaFree(sorted_poa_local_edge_count_d_));
    CU_CHECK_ERR(cudaFree(consensus_scores_d_));
    CU_CHECK_ERR(cudaFree(consensus_predecessors_d_));
    CU_CHECK_ERR(cudaFree(node_marks_d_));
    CU_CHECK_ERR(cudaFree(check_aligned_nodes_d_));
    CU_CHECK_ERR(cudaFree(nodes_to_visit_d_));
}

uint32_t Batch::batch_id() const
{
    return bid_;
}

uint32_t Batch::get_total_poas() const
{
    return poa_count_;
}

void Batch::generate_poa()
{
    CU_CHECK_ERR(cudaSetDevice(device_id_));
    //Copy sequencecs, sequence lengths and window details to device
    CU_CHECK_ERR(cudaMemcpyAsync(inputs_d_, inputs_h_,
                                 num_nucleotides_copied_ * sizeof(uint8_t), cudaMemcpyHostToDevice, stream_));
    CU_CHECK_ERR(cudaMemcpyAsync(window_details_d_, window_details_h_,
                                 poa_count_ * sizeof(nvidia::cudapoa::WindowDetails), cudaMemcpyHostToDevice, stream_));
    CU_CHECK_ERR(cudaMemcpyAsync(sequence_lengths_d_, sequence_lengths_h_,
                                 global_sequence_idx_ * sizeof(uint16_t), cudaMemcpyHostToDevice, stream_));

    // Launch kernel to run 1 POA per thread in thread block.
    std::cerr << TABS << bid_ << " Launching kernel for " << poa_count_ << std::endl;
    nvidia::cudapoa::generatePOA(consensus_d_,
                                 inputs_d_,
                                 sequence_lengths_d_,
                                 window_details_d_,
                                 poa_count_,
                                 NUM_THREADS,
                                 poa_count_,
                                 stream_,
                                 scores_d_,
                                 alignment_graph_d_,
                                 alignment_read_d_,
                                 nodes_d_,
                                 incoming_edges_d_,
                                 incoming_edge_count_d_,
                                 outgoing_edges_d_,
                                 outgoing_edge_count_d_,
                                 incoming_edges_weights_d_,
                                 outoing_edges_weights_d_,
                                 sorted_poa_d_,
                                 sorted_poa_node_map_d_,
                                 node_alignments_d_,
                                 node_alignment_count_d_,
                                 sorted_poa_local_edge_count_d_,
                                 consensus_scores_d_,
                                 consensus_predecessors_d_,
                                 node_marks_d_,
                                 check_aligned_nodes_d_,
                                 nodes_to_visit_d_);
    CU_CHECK_ERR(cudaPeekAtLastError());
    std::cerr << TABS << bid_ << " Launched kernel" << std::endl;
}

const std::vector<std::string>& Batch::get_consensus()
{
    std::cerr << TABS << bid_ << " Launching memcpy D2H" << std::endl;
    CU_CHECK_ERR(cudaMemcpy2DAsync(consensus_h_.get(),
				   CUDAPOA_MAX_SEQUENCE_SIZE,
				   consensus_d_,
				   consensus_pitch_,
				   CUDAPOA_MAX_SEQUENCE_SIZE,
				   max_poas_,
				   cudaMemcpyDeviceToHost,
				   stream_));
    CU_CHECK_ERR(cudaStreamSynchronize(stream_));

    std::cerr << TABS << bid_ << " Finished memcpy D2H" << std::endl;

    for(uint32_t poa = 0; poa < poa_count_; poa++)
    {
        char* c = reinterpret_cast<char *>(&consensus_h_[poa * CUDAPOA_MAX_SEQUENCE_SIZE]);
        std::string reversed_consensus = std::string(c);
        std::reverse(reversed_consensus.begin(), reversed_consensus.end());
        consensus_strings_.push_back(reversed_consensus);
    }

    return consensus_strings_;
}

void Batch::set_cuda_stream(cudaStream_t stream)
{
    stream_ = stream;
}

void Batch::set_device_id(uint32_t device_id)
{
    device_id_ = device_id;
}

void Batch::add_poa()
{
    if (poa_count_ == max_poas_)
    {
        throw std::runtime_error("Maximum POAs already added to batch.");
    }
    WindowDetails window_details{};
    window_details.seq_len_buffer_offset = global_sequence_idx_;
    window_details.seq_starts = num_nucleotides_copied_;
    window_details_h_[poa_count_] = window_details;
    poa_count_++;
}

void Batch::reset()
{
    poa_count_ = 0;
    num_nucleotides_copied_ = 0;
    global_sequence_idx_ = 0;
    consensus_strings_.clear();

    // Clear host and device memory.
    memset(&inputs_h_[0], 0, max_poas_ * max_sequences_per_poa_ * CUDAPOA_MAX_SEQUENCE_SIZE);
    CU_CHECK_ERR(cudaMemsetAsync(inputs_d_, 0, max_poas_ * max_sequences_per_poa_ * CUDAPOA_MAX_SEQUENCE_SIZE, stream_));
}

void Batch::add_seq_to_poa(const char* seq, uint32_t seq_len)
{
    if (seq_len >= CUDAPOA_MAX_SEQUENCE_SIZE)
    {
        throw std::runtime_error("Inserted sequence is larger than maximum sequence size.");
    }

    WindowDetails *window_details = &window_details_h_[poa_count_ - 1];
    window_details->num_seqs++;

    if (window_details->num_seqs == max_sequences_per_poa_)
    {
        throw std::runtime_error("Number of sequences in POA larger than max specified.");
    }

    memcpy(&(inputs_h_[num_nucleotides_copied_]),
           seq,
           seq_len);
    sequence_lengths_h_[global_sequence_idx_] = seq_len;

    num_nucleotides_copied_ += seq_len;
    global_sequence_idx_++;
}

}

}
