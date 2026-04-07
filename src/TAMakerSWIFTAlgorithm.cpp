#include "triggeralgs/SWIFT/TAMakerSWIFTAlgorithm.hpp"

#include "TRACE/trace.h"
#define TRACE_NAME "TriggerActivityMakerSWIFTPlugin"

#include <cassert>
#include <iostream>
#include <cmath>

namespace triggeralgs {

  // configuration
  void TriggerActivityMakerSWIFT::configure(const nlohmann::json& config)
  {
    //window settings
    if (config.contains("window_length"))
      m_window_length = config["window_length"];

    if (config.contains("inspect_energy_threshold"))
      m_inspect_energy_threshold = config["inspect_energy_threshold"];

    if (config.contains("accept_energy_threshold"))
      m_accept_energy_threshold = config["accept_energy_threshold"];

    //tp filtering settings
    if (config.contains("min_adc_peak"))
      m_min_adc_peak = config["min_adc_peak"];

    if (config.contains("min_samples_over_threshold"))
      m_min_samples_over_threshold = config["min_samples_over_threshold"];

    //clustering
    if (config.contains("cm_per_tick"))
      m_cm_per_tick = config["cm_per_tick"];

    if (config.contains("wire_pitch"))
      m_wire_pitch = config["wire_pitch"];

    if (config.contains("min_samples"))
      m_db_min_samples = config["min_samples"];

    if (config.contains("epsilon")) // NN search radius
      m_db_eps = config["epsilon"];

    if (config.contains("cluster_energy_cut"))
      m_cluster_energy_cut = config["cluster_energy_cut"];

    assert(m_window_length > 0);
  }

  //TP refinement
  bool TriggerActivityMakerSWIFT::preprocess( const TriggerPrimitive& input_tp){
    if ((input_tp.adc_peak < m_min_adc_peak) && (input_tp.samples_over_threshold < m_min_samples_over_threshold )) {
      return false;
    }
    return true;
  }

  // Reset window state
  void TriggerActivityMakerSWIFT::reset_window_state(uint64_t new_window_start) {
    m_window_start  = new_window_start;
    m_window_energy = 0;
    m_tp_count      = 0;
    m_current_ta    = TriggerActivity(); //.inputs.clear();  // reuse existing TA heap allocation
    m_current_ta.time_start = m_window_start;
  }

  //main function for binning TPs into fixed-size time windows
  //assumes TPs are strictly time-ordered
  void TriggerActivityMakerSWIFT::process(const TriggerPrimitive& input_tp, std::vector<TriggerActivity>& output_tas)
  {

    // Apply TP filtering
    if (!preprocess(input_tp)) {
      return;
    }

    // If TP is valid, determine which window this TP belongs to
    const uint64_t tp_window_start = (input_tp.time_start / m_window_length) * m_window_length;

    // Initialise on first TP
    if (!m_initialised) {

      //create TA instance once at run start
      m_current_ta = TriggerActivity();

      //reset window state
      reset_window_state(tp_window_start);
      m_initialised = true;
    }

    //if TP is tardy and belongs to past window - reject it for now
    if (tp_window_start < m_window_start){
      return;
    }

    //if TP belongs to future window, close current and jump ahead
    if (tp_window_start > m_window_start){
      close_window(output_tas);
      reset_window_state(tp_window_start);
    }

    //If we got here, the TP belongs to the current window
    m_current_ta.inputs.push_back(input_tp);
    m_window_energy += input_tp.adc_integral;
    ++m_tp_count;
  }

  //function which gets called when the window is closed.
  //main window categorisation & TA generation happens here
  void TriggerActivityMakerSWIFT::close_window(std::vector<TriggerActivity>& output_tas) {

    if (m_current_ta.inputs.empty()) return;

    //Prompt window categorisatoin : immidiate accept, inspect, reject based on local energy in window
    WindowDecision decision;
    if (m_window_energy >= m_accept_energy_threshold) decision = WindowDecision::Accept;
    else if (m_window_energy >= m_inspect_energy_threshold) decision = WindowDecision::Inspect;
    else  return; // Reject

    // cluster inspect cases
    if (decision == WindowDecision::Inspect) {
      const uint64_t max_cluster_energy =  extract_dominant_cluster_energy(m_current_ta.inputs, m_db_eps, m_db_min_samples);
      if (max_cluster_energy <= m_cluster_energy_cut) return; // reject window if it didn't pass inspection
    //for the time being, not updating the flag to keep track of what  went through the inspect-->accept pipeline. FIXME?
    }

    // Emit TA: should only reach this step if dealing with  Accept, or Inspect windows that passed clustering
    set_ta_attributes();
    output_tas.push_back(m_current_ta);
  }



  //Clustering function: density-based clustering in t-z, where both coordinates are expressed in cm.
  //returns only max eng. for now, as that's the param. based on which decision is made.
  //FIXME eventually want this step to return nclusrers, mean cluster eng. etc.
  uint64_t TriggerActivityMakerSWIFT::extract_dominant_cluster_energy(const std::vector<TriggerPrimitive>& tps, float eps, int min_samples){

    struct Point {
      float z;
      float t;
      uint64_t adc;
    };

    //since time and channel are in different units, express TPs as points in unified coordinate system (z,t)
    std::vector<Point> points;
    points.reserve(tps.size());
    for (const auto& tp : tps) {
      points.push_back({tp.channel * m_wire_pitch, (tp.time_start - m_window_start) * m_cm_per_tick, tp.adc_integral});
    }

    const int N = points.size(); //number of elements to cluster
    int cluster_id = 0;
    std::vector<int> labels(N, -1); // initialise all labels to noise for now
    std::vector<uint8_t> visited(N, 0);
    float eps2 = eps * eps; //clustering radius

    for (int i = 0; i < N; ++i) {
      if (visited[i]) continue;
      visited[i] = 1;

      std::vector<int> neigh;
      const float zi = points[i].z;
      const float ti = points[i].t;
      for (int j = 0; j < N; ++j) {
        float dz = points[j].z - zi;
        float dt = points[j].t - ti;
        if (dz*dz + dt*dt <= eps2) neigh.push_back(j);
      }

      if (neigh.size() < static_cast<size_t>(min_samples)) {
        labels[i] = -1;
        continue;
      }

      labels[i] = cluster_id;

      size_t k = 0;
      while (k < neigh.size()) {
        int j = neigh[k];

        if (!visited[j]) {
          visited[j] = 1;

          std::vector<int> neigh2;
          const float zj = points[j].z;
          const float tj = points[j].t;
          for (int m = 0; m < N; ++m) {
            float dz = points[m].z - zj;
            float dt = points[m].t - tj;
            if (dz*dz + dt*dt <= eps2) neigh2.push_back(m);
          }
          if (neigh2.size() >= static_cast<size_t>(min_samples)) {
            for (int m : neigh2) {
              if (std::find(neigh.begin(), neigh.end(), m) == neigh.end())
                neigh.push_back(m);
            }
          }
        }

        if (labels[j] == -1) labels[j] = cluster_id;
        ++k;
      }
      ++cluster_id;
    }

    // once clusters are formed, calculate the energies
    std::vector<uint64_t> cluster_sums(cluster_id, 0);
    for (int i = 0; i < N; ++i) {
      if (labels[i] >= 0) cluster_sums[labels[i]] += points[i].adc;
    }
    //return dominant cluster energy in window
    if (cluster_sums.empty()) return 0;
    return *std::max_element(cluster_sums.begin(), cluster_sums.end());
  }


  //TA feature extraction - FIXME most of these fields are somewhat useless
  void TriggerActivityMakerSWIFT::set_ta_attributes()
  {
    m_current_ta.time_start = m_window_start;
    m_current_ta.time_end   = m_window_start + m_window_length;
    m_current_ta.adc_integral = m_window_energy;

    const TriggerPrimitive& first_tp = m_current_ta.inputs.front();
    m_current_ta.detid = first_tp.detid;
    m_current_ta.type = TriggerActivity::Type::kTPC;
    m_current_ta.algorithm = TriggerActivity::Algorithm::kUnknown;


    dunedaq::trgdataformats::channel_t min_ch = first_tp.channel;
    dunedaq::trgdataformats::channel_t max_ch = first_tp.channel;

    // Peak quantities
    m_current_ta.adc_peak = 0;
    for (const auto& tp : m_current_ta.inputs) {
      if (tp.channel < min_ch) min_ch = tp.channel;
      if (tp.channel > max_ch) max_ch = tp.channel;

      if (tp.adc_peak > m_current_ta.adc_peak) {
        m_current_ta.adc_peak = tp.adc_peak;
        m_current_ta.channel_peak = tp.channel;
        m_current_ta.time_peak = tp.time_start;
      }
    }

    m_current_ta.channel_start = min_ch;
    m_current_ta.channel_end   = max_ch;
    m_current_ta.time_activity = m_current_ta.time_peak;

  }


  void TriggerActivityMakerSWIFT::flush(timestamp_t /*until*/, std::vector<TriggerActivity>& output_tas)
  {
    if (!m_initialised) return;
    close_window(output_tas);
  }

  REGISTER_TRIGGER_ACTIVITY_MAKER(TRACE_NAME, TriggerActivityMakerSWIFT)

} // namespace triggeralgs
