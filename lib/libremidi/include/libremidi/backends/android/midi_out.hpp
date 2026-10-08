#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

NAMESPACE_LIBREMIDI
{
namespace android
{
class midi_out final
    : public midi1::out_api
    , public error_handler
    , public target
{
public:
  //! Messages sent between open_port() and the end of the asynchronous device open are
  //! queued (up to this many bytes) and flushed, in order, once the device is open.
  static constexpr std::size_t max_pending_bytes = 64 * 1024;

  explicit midi_out(
      libremidi::output_configuration&& conf, libremidi::android::output_configuration aconf)
      : configuration{std::move(conf)}
  {
    if (!context::client_name.empty() && context::client_name != aconf.client_name)
    {
      libremidi_handle_error(
          configuration, "Android backend only supports one client name per process");
      return;
    }
    context::client_name = std::string(aconf.client_name);
    client_open_ = stdx::error{};
  }

  ~midi_out() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  stdx::error open_port(const output_port& port, std::string_view /*name*/) override
  {
    if (port_open)
    {
      if (!lost)
      {
        libremidi_handle_error(configuration, "midi_out::open_port: a port is already open");
        return std::errc::device_or_resource_busy;
      }
      close_port(); // the device behind the previous port is gone: release it first
    }

    // An earlier open that failed asynchronously leaves its token registered (the application
    // may retry without close_port()): drop it, or it would dangle once this port is destroyed.
    if (const jlong stale = token.exchange(0))
      context::unregister_target(stale);

    const unsigned int port_number = port.port;

    auto env = context::get_thread_env();
    if (!env)
    {
      libremidi_handle_error(configuration, "midi_out::open_port: failed to get JNI environment");
      return std::errc::io_error;
    }

    auto ctx = context::get_context(env);
    if (!ctx)
    {
      libremidi_handle_error(configuration, "midi_out::open_port: failed to get Android context");
      return std::errc::io_error;
    }

    context::refresh_midi_devices(env, ctx, true);

    if (port_number >= context::midi_devices.size())
    {
      libremidi_handle_error(configuration, "midi_out::open_port: invalid port number");
      return std::errc::invalid_argument;
    }

    {
      std::lock_guard lock{m_mutex};
      port_open = true;
      lost = false;
      loss_reported = false;
      pending.clear();
      pending_bytes = 0;
      queued_warned = overflow_reported = false;
      opened_at = std::chrono::steady_clock::now();
    }

    const int device_id = port_number < context::midi_device_ids.size()
                              ? context::midi_device_ids[port_number]
                              : -1;
    const jlong new_token = context::register_target(
        this, device_id, configuration.on_error, configuration.on_warning);
    token = new_token;
    if (!context::open_device(
            context::midi_devices[port_number], reinterpret_cast<void*>(new_token), true))
    {
      close_port();
      libremidi_handle_error(configuration, "midi_out::open_port: failed to open the device");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    // After this returns, no open callback is running and none will run: a late
    // MidiDeviceCallback finds no target and releases its device.
    if (const jlong t = token.exchange(0))
      context::unregister_target(t);

    std::lock_guard lock{m_mutex};
    if (!port_open)
      return stdx::error{};

    if (midi_input_port)
    {
      AMidiInputPort_close(midi_input_port);
      midi_input_port = nullptr;
    }

    if (send_device)
    {
      context::release_device(send_device);
      send_device = nullptr;
    }

    pending.clear();
    pending_bytes = 0;
    port_open = false;
    return stdx::error{};
  }

  bool is_port_open() const noexcept { return port_open && !lost; }

  stdx::error send_message(const unsigned char* message, size_t size) override
  {
    std::errc code{};
    report rep;
    {
      std::lock_guard lock{m_mutex};
      if (!port_open)
      {
        code = std::errc::not_connected;
        rep = {true, "midi_out::send_message: port is not open"};
      }
      else if (lost)
      {
        // Already reported when the loss was detected.
        code = std::errc::not_connected;
      }
      else if (!midi_input_port)
      {
        // open_port() returned but Java has not delivered the device yet.
        if (pending_bytes + size > max_pending_bytes)
        {
          code = std::errc::no_buffer_space;
          if (!overflow_reported)
          {
            overflow_reported = true;
            rep = {true, "midi_out::send_message: output not open yet and the queue is full, dropping"};
          }
        }
        else
        {
          pending.emplace_back(message, message + size);
          pending_bytes += size;
          if (!queued_warned)
          {
            queued_warned = true;
            rep = {false, "midi_out::send_message: device still opening, message queued"};
          }
        }
      }
      else
      {
        const ssize_t result = AMidiInputPort_send(midi_input_port, message, size);
        if (result < 0)
        {
          // The framework no longer accepts data: the device is gone. Latch it so that later
          // sends fail fast and the application is told once.
          code = std::errc::io_error;
          mark_lost();
          if (!loss_reported.exchange(true))
            rep = {
                true, "midi_out::send_message: failed to send MIDI data (" + std::to_string(result)
                          + "), device lost"};
        }
        else if (static_cast<size_t>(result) != size)
        {
          code = std::errc::io_error;
          rep = {true, "midi_out::send_message: MIDI data was only partially sent"};
        }
      }
    }

    // User callbacks run without the lock so that they may close the port.
    if (!rep.text.empty())
    {
      if (rep.is_error)
        error_impl(configuration.on_error, rep.text, libremidi::source_location::current());
      else
        warning_impl(configuration.on_warning, rep.text, libremidi::source_location::current());
    }
    return code;
  }

  void on_device_opened(AMidiDevice* device, std::vector<report>& out) override
  {
    std::lock_guard lock{m_mutex};
    if (!port_open || lost)
    {
      // Closed, or the device was removed while it was still opening: nothing to open or
      // flush, and nothing more to report (the loss was reported when it was detected).
      context::release_device(device);
      return;
    }

    send_device = device;
    const media_status_t status = AMidiInputPort_open(device, 0, &midi_input_port);
    if (status != AMEDIA_OK || !midi_input_port)
    {
      midi_input_port = nullptr;
      fail_open_locked(
          out, "midi_out::open_port: AMidiInputPort_open failed: " + std::to_string((int)status));
      return;
    }

    if (pending.empty())
      return;

    std::size_t flushed = 0, bytes = 0;
    for (const auto& m : pending)
    {
      const ssize_t result = AMidiInputPort_send(midi_input_port, m.data(), m.size());
      if (result < 0 || static_cast<size_t>(result) != m.size())
      {
        out.push_back({true, "midi_out: failed to send a queued MIDI message"});
        break;
      }
      flushed++;
      bytes += m.size();
    }
    const auto waited = std::chrono::steady_clock::now() - opened_at;
    out.push_back(
        {false,
         "midi_out: flushed " + std::to_string(flushed) + " of " + std::to_string(pending.size())
             + " queued message(s) (" + std::to_string(bytes) + " bytes) "
             + std::to_string(
                 std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count())
             + " ns after open_port"});
    pending.clear();
    pending_bytes = 0;
  }

  void on_open_failed(std::vector<report>& out) override
  {
    std::lock_guard lock{m_mutex};
    if (!port_open)
      return;
    fail_open_locked(out, "midi_out::open_port: the MIDI device could not be opened");
  }

  void on_device_lost(std::vector<report>& out) override
  {
    // Lock order: registry (held by the caller) -> m_mutex, as in on_device_opened().
    std::lock_guard lock{m_mutex};
    if (!port_open)
      return;
    pending.clear(); // never to be flushed
    pending_bytes = 0;
    mark_lost();
    if (!loss_reported.exchange(true))
      out.push_back({true, "midi_out: the MIDI device was removed"});
  }

private:
  void mark_lost()
  {
    lost = true;
    store_relaxed(port_open_, false); // what libremidi::midi_out::is_port_open() reads
  }

  // m_mutex must be held.
  void fail_open_locked(std::vector<report>& out, std::string message)
  {
    if (send_device)
    {
      context::release_device(send_device);
      send_device = nullptr;
    }
    pending.clear();
    pending_bytes = 0;
    port_open = false;
    store_relaxed(port_open_, false); // what libremidi::midi_out::is_port_open() reads
    out.push_back({true, std::move(message)});
  }

  libremidi::output_configuration configuration;

  std::mutex m_mutex; // everything below, between the sender, the app thread and the Java thread
  AMidiDevice* send_device = nullptr;
  AMidiInputPort* midi_input_port = nullptr;
  std::atomic<bool> port_open{false};
  std::atomic<bool> lost{false};          // the device behind the port was removed
  std::atomic<bool> loss_reported{false}; // on_error was told about it
  std::deque<std::vector<unsigned char>> pending;
  std::size_t pending_bytes = 0;
  bool queued_warned = false;
  bool overflow_reported = false;
  std::chrono::steady_clock::time_point opened_at;

  std::atomic<jlong> token{0};
};
}
}
