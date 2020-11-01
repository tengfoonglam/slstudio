#include "ProjectorLC4500_versavis.h"

#include <QThread>
#include <iostream>

#include "LC4500API/dlpc350_api.h"
#include "LC4500API/dlpc350_usb.h"

#include <chrono>
#include <thread>

ProjectorLC4500_versavis::ProjectorLC4500_versavis(unsigned int)
    : nPatterns(0), isRunning(false), m_projector() {}

void ProjectorLC4500_versavis::init() {
  if (m_is_in_calibration_mode && m_is_hardware_triggered) {
    show_error(
        "Warning: Hardware trigger is enabled for calibration mode. This is "
        "not a possible combination. Configuring to be software triggered");
    m_is_hardware_triggered = false;
  }

  lc4500_init();

  if (m_is_hardware_triggered) {
    // Ros initialisation
    ros_init();

    // Load entire pattern sequence instructions onto projector
    m_projector.set_pattern_sequence(m_pattern_sequence);
    m_projector.send_pattern_sequence(m_hardware_triggered_timings_us[0],
                                      m_hardware_triggered_timings_us[1]);
  }
}

void ProjectorLC4500_versavis::setPattern(unsigned int patternNumber,
                                          const unsigned char *tex,
                                          unsigned int texWidth,
                                          unsigned int texHeight) {}

void ProjectorLC4500_versavis::displayPattern(unsigned int pattern_no) {
  // If it is the first pattern being triggered for the first time, we detect
  // the start of the trigger before we end the function to ensure
  // synchronisation
  if (m_is_hardware_triggered && pattern_no == 0) {
    unsigned int current_count;
    unsigned int updated_count;

    {
      boost::mutex::scoped_lock lock(m_mutex);
      current_count = m_counter;
    }
    updated_count = current_count;

    {
      boost::mutex::scoped_lock lock(m_mutex);
      updated_count = m_counter;
    }

    std::cout << "Waiting for hardware trigger to be detected ";
    // Wait until count is updated (versavis has triggered)
    while (current_count == updated_count) {
      std::cout << ".";

      // Sleep for half a ms
      std::this_thread::sleep_for(std::chrono::microseconds((uint32_t)(500)));
      // Check if there are any updates to m_counter
      {
        boost::mutex::scoped_lock lock(m_mutex);
        updated_count = m_counter;
      }
    }
    std::cout << std::endl;

    std::cout << "Started playing image sequence (hardware triggered)!"
              << std::endl;

    if (!m_first_time_hardware_triggered) {
      m_projector.set_pat_seq_start();
      m_first_time_hardware_triggered = true;
    }
  }

  if (!m_is_hardware_triggered) {
    // If not hardware triggered we upload pattern display instructions every
    // time we display a new pattern

    std::vector<single_pattern> temp_vec = {};
    temp_vec.push_back(m_pattern_sequence[pattern_no]);
    m_projector.play_pattern_sequence(temp_vec,
                                      m_software_trigger_timings_us[0],
                                      m_software_trigger_timings_us[1]);
  }
}

void ProjectorLC4500_versavis::displayTexture(const unsigned char *tex,
                                              unsigned int texWidth,
                                              unsigned int texHeight) {}

void ProjectorLC4500_versavis::displayBlack() {}

void ProjectorLC4500_versavis::displayWhite() {}

void ProjectorLC4500_versavis::getScreenRes(unsigned int *nx,
                                            unsigned int *ny) {
  // Native resolution of Lightcrafter 4500
  *nx = 912;
  *ny = 1140;
}

ProjectorLC4500_versavis::~ProjectorLC4500_versavis() {
  // std::cout << "ProjectorLC4500_versavis destructor called" << std::endl;
  m_projector.close();
  if (m_is_hardware_triggered && !m_is_in_calibration_mode) {
    m_spinner_ptr->stop();
    ros::shutdown();
  }
  // std::cout << "ProjectorLC4500_versavis destructor completed" << std::endl;
}

void ProjectorLC4500_versavis::sub_cb(
    const versavis::TimeNumberedConstPtr &time_numbered_ptr) {
  // std::cout << "Message received!" << std::endl;

  boost::mutex::scoped_lock lock(m_mutex);
  m_counter = time_numbered_ptr->number;

  // std::cout << "Versavis counter: " << m_counter << std::endl;
}

void ProjectorLC4500_versavis::ros_init() {
  // Init ros node
  ros::M_string empty_args = {};
  ros::init(empty_args, m_ros_node_name);
  m_nh_ptr = std::make_unique<ros::NodeHandle>();

  // Set up subscriber
  m_sub = m_nh_ptr->subscribe(m_projector_trigger_topic, 5,
                              &ProjectorLC4500_versavis::sub_cb, this);

  // We use an Asyncrhonous Spinner since we dont have access to the main loop
  // Spinning is required or else subscriber will never receive messages
  m_spinner_ptr = std::make_unique<ros::AsyncSpinner>(1);
  m_spinner_ptr->start();
}

void ProjectorLC4500_versavis::lc4500_init() {
  if (m_projector.init() < 0) {
    std::cout << "Error, failed to initialise projector" << std::endl;
    throw;
  }

  // Set LED Brightness
  m_projector.set_led_currents(m_rgb_white[0], m_rgb_white[1], m_rgb_white[2]);

  // Load which pattern sequence to display (calibration or scanning)
  load_pattern_sequence();
}

void ProjectorLC4500_versavis::show_error(const std::string &err) {
  std::cerr << "Lightcrafter-Versavis Projectror: " << err.c_str() << std::endl;
}

void ProjectorLC4500_versavis::load_param(const std::string &param_name,
                                          std::shared_ptr<void> param_ptr) {
  if (param_name == "is_hardware_triggered") {
    std::shared_ptr<bool> temp_ptr;
    temp_ptr = std::static_pointer_cast<bool>(param_ptr);
    m_is_hardware_triggered = *temp_ptr;
    // std::cout << "m_is_hardware_triggered: " << m_is_hardware_triggered
    //          << std::endl;
  } else if (param_name == "is_in_calibration_mode") {
    std::shared_ptr<bool> temp_ptr;
    temp_ptr = std::static_pointer_cast<bool>(param_ptr);
    m_is_in_calibration_mode = *temp_ptr;
    // std::cout << "m_is_in_calibration_mode: " << m_is_in_calibration_mode
    //          << std::endl;
  } else if (param_name == "display_horizontal_pattern") {
    std::shared_ptr<bool> temp_ptr;
    temp_ptr = std::static_pointer_cast<bool>(param_ptr);
    m_display_horizontal_pattern = *temp_ptr;
  } else if (param_name == "display_vertical_pattern") {
    std::shared_ptr<bool> temp_ptr;
    temp_ptr = std::static_pointer_cast<bool>(param_ptr);
    m_display_vertical_pattern = *temp_ptr;
  }
}

std::vector<single_pattern>
ProjectorLC4500_versavis::get_calibration_pattern_sequence() {
  std::vector<single_pattern> pattern_vec = {};

  for (int i = 0; i < m_calibration_image_indices.size(); i++) {
    for (int j = 0; j < 3; j++) {
      single_pattern temp;
      temp.trigger_type = 0;
      temp.pattern_number = j;
      temp.bit_depth = 8;
      temp.led_select = 7;
      temp.image_indice = m_calibration_image_indices[i];
      temp.invert_pattern = false;
      temp.insert_black_frame = false;
      temp.buffer_swap = true;
      temp.trigger_out_prev = false;
      pattern_vec.push_back(temp);
    }
  }

  return pattern_vec;
}

// TrigType  - I - Select the trigger type for the pattern
//                          0 = Internal trigger
//                          1 = External positive
//                          2 = External negative
//                          3 = No Input Trigger

std::vector<single_pattern>
ProjectorLC4500_versavis::get_scanning_pattern_sequence_software() {
  std::vector<single_pattern> pattern_vec = {};

  std::vector<int> images_indices_to_display;

  if (m_display_horizontal_pattern) {
    images_indices_to_display.push_back(m_scanning_image_indices[0]);
    images_indices_to_display.push_back(m_scanning_image_indices[1]);
  }

  if (m_display_vertical_pattern) {
    images_indices_to_display.push_back(m_scanning_image_indices[2]);
    images_indices_to_display.push_back(m_scanning_image_indices[3]);
  }

  for (int i = 0; i < images_indices_to_display.size(); i++) {
    for (int j = 0; j < 3; j++) {
      single_pattern temp;
      temp.trigger_type = 0;
      temp.pattern_number = j;
      temp.bit_depth = 8;
      temp.led_select = 7;
      temp.image_indice = images_indices_to_display[i];
      temp.invert_pattern = false;
      temp.insert_black_frame = false;
      temp.buffer_swap = true;
      temp.trigger_out_prev = false;
      pattern_vec.push_back(temp);
    }
  }

  return pattern_vec;
}

std::vector<single_pattern>
ProjectorLC4500_versavis::get_scanning_pattern_sequence_hardware() {
  std::vector<single_pattern> pattern_vec = {};

  std::vector<int> images_indices_to_display;

  if (m_display_horizontal_pattern) {
    images_indices_to_display.push_back(m_scanning_image_indices[0]);
    images_indices_to_display.push_back(m_scanning_image_indices[1]);
  }

  if (m_display_vertical_pattern) {
    images_indices_to_display.push_back(m_scanning_image_indices[2]);
    images_indices_to_display.push_back(m_scanning_image_indices[3]);
  }

  for (int i = 0; i < images_indices_to_display.size(); i++) {
    for (int j = 0; j < 3; j++) {
      single_pattern temp1;
      temp1.trigger_type = (i == 0 && j == 0) ? 1 : 3;
      temp1.pattern_number = j;
      temp1.bit_depth = 8;
      temp1.led_select = 7;
      temp1.image_indice = images_indices_to_display[i];
      temp1.invert_pattern = false;
      temp1.insert_black_frame = false;
      temp1.buffer_swap = (j == 0) ? true : false;
      temp1.trigger_out_prev = false;
      pattern_vec.push_back(temp1);

      single_pattern temp2;
      temp2.trigger_type = 3;
      temp2.pattern_number = j;
      temp2.bit_depth = 8;
      temp2.led_select = 7;
      temp2.image_indice = images_indices_to_display[i];
      temp2.invert_pattern = false;
      temp2.insert_black_frame = false;
      temp2.buffer_swap = false;
      temp2.trigger_out_prev = false;
      pattern_vec.push_back(temp2);
    }
  }

  return pattern_vec;
}

void ProjectorLC4500_versavis::load_pattern_sequence() {
  m_pattern_sequence = (m_is_in_calibration_mode)
                           ? get_calibration_pattern_sequence()
                           : (m_is_hardware_triggered)
                                 ? get_scanning_pattern_sequence_hardware()
                                 : get_scanning_pattern_sequence_software();
}
