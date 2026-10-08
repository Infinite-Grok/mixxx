#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/midi_in.hpp>
#include <libremidi/detail/midi_stream_decoder.hpp>

#include <atomic>
#include <chrono>
#include <thread>

NAMESPACE_LIBREMIDI
{
namespace android
{
class midi_in
    : public midi1::in_api
    , public error_handler
    , public target
{
public:
  explicit midi_in(
      libremidi::input_configuration&& conf, libremidi::android::input_configuration aconf)
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

  ~midi_in() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  stdx::error open_port(const input_port& port, std::string_view /*name*/) override
  {
    if (port_open)
    {
      if (!lost)
      {
        libremidi_handle_error(configuration, "midi_in::open_port: a port is already open");
        return std::errc::device_or_resource_busy;
      }
      close_port(); // the device behind the previous port is gone: release it first
    }

    const unsigned int port_number = port.port;
    auto env = context::get_thread_env();
    if (!env)
    {
      libremidi_handle_error(configuration, "midi_in::open_port: failed to get JNI environment");
      return std::errc::io_error;
    }

    auto ctx = context::get_context(env);
    if (!ctx)
    {
      libremidi_handle_error(configuration, "midi_in::open_port: failed to get Android context");
      return std::errc::io_error;
    }

    context::refresh_midi_devices(env, ctx, false);

    if (port_number >= context::midi_devices.size())
    {
      libremidi_handle_error(configuration, "midi_in::open_port: invalid port number");
      return std::errc::invalid_argument;
    }

    port_open = true;
    running = true;
    lost = false;
    loss_reported = false;
    const int device_id = port_number < context::midi_device_ids.size()
                              ? context::midi_device_ids[port_number]
                              : -1;
    token = context::register_target(
        this, device_id, configuration.on_error, configuration.on_warning);
    if (!context::open_device(
            context::midi_devices[port_number], reinterpret_cast<void*>(token), false))
    {
      close_port();
      libremidi_handle_error(configuration, "midi_in::open_port: failed to open the device");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    // After this returns, no open callback is running and none will run: a late
    // MidiDeviceCallback finds no target and releases its device.
    if (token != 0)
    {
      context::unregister_target(token);
      token = 0;
    }

    if (!port_open)
      return stdx::error{};

    running = false;

    if (poll_thread.joinable())
    {
      // close_port() may be called from an on_error callback, i.e. from the poll thread itself.
      if (poll_thread.get_id() == std::this_thread::get_id())
        poll_thread.detach();
      else
        poll_thread.join();
    }

    if (midi_output_port)
    {
      AMidiOutputPort_close(midi_output_port);
      midi_output_port = nullptr;
    }

    if (receive_device)
    {
      AMidiDevice_release(receive_device);
      receive_device = nullptr;
    }

    port_open = false;
    return stdx::error{};
  }

  bool is_port_open() const noexcept { return port_open && !lost; }

  void start_midi_thread() { poll_thread = std::thread(&midi_in::poll_midi, this); }

  timestamp absolute_timestamp() const noexcept override { return system_ns(); }
  void poll_midi()
  {
    static constexpr timestamp_backend_info timestamp_info{
        .has_absolute_timestamps = false,
        .absolute_is_monotonic = false,
        .has_samples = false,
    };

    uint8_t buffer[1024];
    int32_t op_code;
    int64_t timestamp;
    size_t num_bytes;

    while (running)
    {
      // AMidi has no blocking receive, so wake up periodically...
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

      // The device was removed (already reported by on_device_lost()): nothing to poll.
      if (lost)
        return;

      // ... and drain everything that is queued before sleeping again:
      // a single read per wake-up caps throughput at ~1 packet/ms, which a
      // busy controller exceeds, overflowing the framework socket.
      while (running)
      {
        const ssize_t num_messages = AMidiOutputPort_receive(
            midi_output_port, &op_code, buffer, sizeof(buffer), &num_bytes, &timestamp);

        if (num_messages < 0)
        {
          // The port is dead (e.g. the framework dropped the connection): this
          // is not recoverable, so report it once and stop polling.
          mark_lost();
          if (!loss_reported.exchange(true))
            libremidi_handle_error(
                configuration,
                "Error receiving MIDI data: " + std::to_string((int)num_messages)
                    + ", input stopped");
          return;
        }

        if (num_messages == 0)
          break;

        if (op_code == AMIDI_OPCODE_DATA && num_bytes > 0)
        {
          const auto to_ns = [=] { return timestamp; };

          m_processing.on_bytes_multi(
              {buffer, buffer + num_bytes},
              m_processing.timestamp<timestamp_info>(to_ns, timestamp));
        }
      }
    }
  }

  void on_device_opened(AMidiDevice* device, std::vector<report>& out) override
  {
    if (!port_open)
    {
      AMidiDevice_release(device);
      return;
    }

    receive_device = device;
    const media_status_t status = AMidiOutputPort_open(device, 0, &midi_output_port);
    if (status != AMEDIA_OK || !midi_output_port)
    {
      midi_output_port = nullptr;
      fail_open(
          out, "midi_in::open_port: AMidiOutputPort_open failed: " + std::to_string((int)status));
      return;
    }
    start_midi_thread();
  }

  void on_open_failed(std::vector<report>& out) override
  {
    if (!port_open)
      return;
    fail_open(out, "midi_in::open_port: the MIDI device could not be opened");
  }

  void on_device_lost(std::vector<report>& out) override
  {
    if (!port_open)
      return;
    // The poll thread notices `lost` within a millisecond and stops.
    mark_lost();
    if (!loss_reported.exchange(true))
      out.push_back({true, "midi_in: the MIDI device was removed, input stopped"});
  }

private:
  void mark_lost()
  {
    lost = true;
    store_relaxed(port_open_, false); // what libremidi::midi_in::is_port_open() reads
  }

  void fail_open(std::vector<report>& out, std::string message)
  {
    if (receive_device)
    {
      AMidiDevice_release(receive_device);
      receive_device = nullptr;
    }
    running = false;
    port_open = false;
    store_relaxed(port_open_, false); // what libremidi::midi_in::is_port_open() reads
    out.push_back({true, std::move(message)});
  }

  libremidi::input_configuration configuration;
  AMidiDevice* receive_device = nullptr;
  AMidiOutputPort* midi_output_port = nullptr;
  std::thread poll_thread;
  std::atomic<bool> port_open{false};
  std::atomic<bool> running{false};
  std::atomic<bool> lost{false};          // the device behind the port was removed / died
  std::atomic<bool> loss_reported{false}; // on_error was told about it
  jlong token = 0;
  midi1::input_state_machine m_processing{this->configuration};
};
}
}
