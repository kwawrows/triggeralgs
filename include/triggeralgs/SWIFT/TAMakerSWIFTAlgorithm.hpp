/**
 * @file TAMakerSWIFTAlgorithm.hpp
 *
 * This is part of the DUNE DAQ Application Framework, copyright 2021.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#ifndef TRIGGERALGS_SWIFT_TRIGGERACTIVITYMAKERSWIFT_HPP_
#define TRIGGERALGS_SWIFT_TRIGGERACTIVITYMAKERSWIFT_HPP_

#include "triggeralgs/TriggerActivityFactory.hpp"
#include <algorithm>

namespace triggeralgs {
  
  class TriggerActivityMakerSWIFT : public TriggerActivityMaker {
  public:
    //void operator()(const TriggerPrimitive& input_tp, std::vector<TriggerActivity>& output_tas);
    void process(const TriggerPrimitive& input_tp, std::vector<TriggerActivity>& output_tas) override;
    void configure(const nlohmann::json& config);
    void set_ta_attributes();
    void flush(timestamp_t until, std::vector<TriggerActivity>& output_tas) override; 
    bool preprocess( const TriggerPrimitive& input_tp);

  private:
    TriggerActivity m_current_ta;

    // -- Algortihm configuration --
    // static variables which get set once at run start & kept fixed afterwards 

    //Window settings
    uint64_t m_window_length = 32000; // fixed window size in DTS ticks (32 * 1k readout ticks @ 500ns/tick)
    uint64_t m_inspect_energy_threshold = 15000;
    uint64_t m_accept_energy_threshold = 55000;

    //TP pre-processing
    uint16_t m_min_adc_peak = 80;
    uint16_t m_min_samples_over_threshold = 8; 

    //Clustering settings 
    //these depend on detector properties. default values for HD
    float m_cm_per_tick = 0.016 * 0.16; // sampling rate [us/tick] * drift velocity [cm/us] 
    float m_wire_pitch = 0.48; //cm 
    int   m_db_min_samples = 2; //min samples for valid cluster 
    float m_db_eps = 2; //nearest neighbour search radius in cm 
    uint64_t m_cluster_energy_cut = 22000; // min energy of dominant cluster for window to be accepted


    // Dynamic vaiables which get updated during running to keep track of alg state. These are non-configurable
    //window state
    bool     m_initialised = false;
    uint64_t m_window_start = 0;
    uint64_t m_window_energy = 0; 
    uint16_t m_tp_count = 0; 


    //Additional SWIFT-specific functions and classes 
    enum class WindowDecision { Reject, Inspect, Accept };
    void close_window(std::vector<TriggerActivity>& output_tas);
    void reset_window_state(uint64_t new_window_start);
    uint64_t extract_dominant_cluster_energy(const std::vector<TriggerPrimitive>& tps, float eps, int min_samples);
};

} // namespace triggeralgs

#endif // TRIGGERALGS_SWIFT_TRIGGERACTIVITYMAKERSWIFT_HPP_
