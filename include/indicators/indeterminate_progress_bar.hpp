#pragma once

#include <indicators/details/stream_helper.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <indicators/color.hpp>
#include <indicators/setting.hpp>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>

namespace indicators {

class IndeterminateProgressBar {
  using Settings =
      std::tuple<option::BarWidth, option::PrefixText, option::PostfixText, option::Start,
                 option::End, option::Fill, option::Lead,
                 option::MaxPostfixTextLen, option::Completed,
                 option::ForegroundColor, option::FontStyles, option::Stream>;

  enum class Direction {
    forward,
    backward
  };

  Direction direction_{Direction::forward};

public:
  template <typename... Args,
            typename std::enable_if<details::are_settings_from_tuple<
                                        Settings, typename std::decay<Args>::type...>::value,
                                    void *>::type = nullptr>
  explicit IndeterminateProgressBar(Args &&... args)
      : settings_(details::get<details::ProgressBarOption::bar_width>(option::BarWidth{100},
                                                                      std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::prefix_text>(
                      option::PrefixText{}, std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::postfix_text>(
                      option::PostfixText{}, std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::start>(option::Start{"["},
                                                                  std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::end>(option::End{"]"},
                                                                std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::fill>(option::Fill{"."},
                                                                 std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::lead>(option::Lead{"<==>"},
                                                                 std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::max_postfix_text_len>(
                      option::MaxPostfixTextLen{0}, std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::completed>(option::Completed{false},
                                                                      std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::foreground_color>(
                      option::ForegroundColor{Color::unspecified}, std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::font_styles>(
                      option::FontStyles{std::vector<FontStyle>{}}, std::forward<Args>(args)...),
                  details::get<details::ProgressBarOption::stream>(
                      option::Stream{std::cout}, std::forward<Args>(args)...)) {
    // starts with [<==>...........]
    // progress_ = 0

    // ends with   [...........<==>]
    //             ^^^^^^^^^^^^^^^^^ bar_width
    //             ^^^^^^^^^^^^ (bar_width - len(lead))
    // progress_ = bar_width - len(lead)
    progress_ = 0;
    max_progress_ = get_value<details::ProgressBarOption::bar_width>()
      - get_value<details::ProgressBarOption::lead>().size() 
      + get_value<details::ProgressBarOption::start>().size()
      + get_value<details::ProgressBarOption::end>().size();
  }

  template <typename T, details::ProgressBarOption id>
  void set_option(details::Setting<T, id> &&setting) {
    static_assert(!std::is_same<T, typename std::decay<decltype(details::get_value<id>(
                                       std::declval<Settings>()))>::type>::value,
                  "Setting has wrong type!");
    std::lock_guard<std::mutex> lock(mutex_);
    get_value<id>() = std::move(setting).value;
  }

  template <typename T, details::ProgressBarOption id>
  void set_option(const details::Setting<T, id> &setting) {
    static_assert(!std::is_same<T, typename std::decay<decltype(details::get_value<id>(
                                       std::declval<Settings>()))>::type>::value,
                  "Setting has wrong type!");
    std::lock_guard<std::mutex> lock(mutex_);
    get_value<id>() = setting.value;
  }

  void set_option(
      const details::Setting<std::string, details::ProgressBarOption::postfix_text> &setting) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_value<details::ProgressBarOption::postfix_text>() = setting.value;
    if (setting.value.length() > get_value<details::ProgressBarOption::max_postfix_text_len>()) {
      get_value<details::ProgressBarOption::max_postfix_text_len>() = setting.value.length();
    }
  }

  void
  set_option(details::Setting<std::string, details::ProgressBarOption::postfix_text> &&setting) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_value<details::ProgressBarOption::postfix_text>() = std::move(setting).value;
    auto &new_value = get_value<details::ProgressBarOption::postfix_text>();
    if (new_value.length() > get_value<details::ProgressBarOption::max_postfix_text_len>()) {
      get_value<details::ProgressBarOption::max_postfix_text_len>() = new_value.length();
    }
  }

  void tick() {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (get_value<details::ProgressBarOption::completed>())
        return;

      progress_ += (direction_ == Direction::forward) ? 1 : -1;
      if (direction_ == Direction::forward && progress_ == max_progress_) {
        // time to go back
        direction_ = Direction::backward;
      } else if (direction_ == Direction::backward && progress_ == 0) {
        direction_ = Direction::forward;
      }
    }
    print_progress();
  }

  bool is_completed() {
    return get_value<details::ProgressBarOption::completed>();
  }

  void mark_as_completed() {
    get_value<details::ProgressBarOption::completed>() = true;
    print_progress();
  }

private:
  template <details::ProgressBarOption id>
  auto get_value() -> decltype((details::get_value<id>(std::declval<Settings &>()).value)) {
    return details::get_value<id>(settings_).value;
  }

  template <details::ProgressBarOption id>
  auto get_value() const
      -> decltype((details::get_value<id>(std::declval<const Settings &>()).value)) {
    return details::get_value<id>(settings_).value;
  }

  size_t progress_{0};
  size_t max_progress_;
  Settings settings_;
  std::chrono::nanoseconds elapsed_;
  std::mutex mutex_;

  template <typename Indicator, size_t count> friend class MultiProgress;
  template <typename Indicator> friend class DynamicProgress;
  std::atomic<bool> multi_progress_mode_{false};

public:
  void print_progress(bool from_multi_progress = false) {
    std::lock_guard<std::mutex> lock{mutex_};

    auto& os = get_value<details::ProgressBarOption::stream>();

    if (multi_progress_mode_ && !from_multi_progress) {
      return;
    }
    if (get_value<details::ProgressBarOption::foreground_color>() != Color::unspecified)
      details::set_stream_color(os, get_value<details::ProgressBarOption::foreground_color>());

    for (auto &style : get_value<details::ProgressBarOption::font_styles>())
      details::set_font_style(os, style);

    os << get_value<details::ProgressBarOption::prefix_text>();

    os << get_value<details::ProgressBarOption::start>();

    details::IndeterminateProgressScaleWriter writer{os,
                                        get_value<details::ProgressBarOption::bar_width>(),
                                        get_value<details::ProgressBarOption::fill>(),
                                        get_value<details::ProgressBarOption::lead>()};
    writer.write(progress_);

    os << get_value<details::ProgressBarOption::end>();

    if (get_value<details::ProgressBarOption::max_postfix_text_len>() == 0)
      get_value<details::ProgressBarOption::max_postfix_text_len>() = 10;
    os << " " << get_value<details::ProgressBarOption::postfix_text>()
              << std::string(get_value<details::ProgressBarOption::max_postfix_text_len>(), ' ')
              << "\r";
    os.flush();
    if (get_value<details::ProgressBarOption::completed>() &&
        !from_multi_progress) // Don't std::endl if calling from MultiProgress
      os << termcolor::reset << std::endl;
  }
};

} // namespace indicators
