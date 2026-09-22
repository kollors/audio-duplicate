using System;
using System.Collections.Generic;
using System.Linq;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace AudioDuplicate
{
    internal sealed class AudioEngine : IDisposable
    {
        private readonly object _gate = new object();
        private WasapiLoopbackCapture _capture;
        private readonly List<OutputWorker> _outputs = new List<OutputWorker>();
        private volatile bool _running;
        private string _lastError = "";

        public bool IsRunning => _running;
        public string LastError { get { lock (_gate) return _lastError; } }

        public static List<AudioDevice> EnumerateRenderDevices()
        {
            using (var enumerator = new MMDeviceEnumerator())
            {
                string defaultId = "";
                try
                {
                    using (var def = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia))
                        defaultId = def.ID;
                }
                catch { }

                return enumerator
                    .EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
                    .Select(d => new AudioDevice
                    {
                        Id = d.ID,
                        Name = d.FriendlyName,
                        IsDefault = d.ID == defaultId
                    })
                    .OrderByDescending(d => d.IsDefault)
                    .ThenBy(d => d.Name, StringComparer.CurrentCultureIgnoreCase)
                    .ToList();
            }
        }

        public bool Start(string sourceDeviceId, ChannelMode sourceMode, IList<OutputRoute> outputs, out string error)
        {
            Stop();
            SetError("");

            if (string.IsNullOrWhiteSpace(sourceDeviceId))
            {
                error = "Выберите основной выход.";
                return false;
            }

            if (outputs == null || outputs.Count == 0)
            {
                error = "Добавьте хотя бы один дополнительный выход.";
                return false;
            }

            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var route in outputs)
            {
                if (string.IsNullOrWhiteSpace(route.DeviceId))
                {
                    error = "Выберите устройство для каждого дополнительного выхода.";
                    return false;
                }

                if (string.Equals(route.DeviceId, sourceDeviceId, StringComparison.Ordinal))
                {
                    error = "Основной и дополнительный выход не должны быть одним устройством.";
                    return false;
                }

                if (!seen.Add(route.DeviceId))
                {
                    error = "Одно дополнительное устройство выбрано несколько раз.";
                    return false;
                }
            }

            try
            {
                var enumerator = new MMDeviceEnumerator();
                var source = enumerator.GetDevice(sourceDeviceId);
                _capture = new WasapiLoopbackCapture(source);
                var sourceFormat = _capture.WaveFormat;

                lock (_gate)
                {
                    foreach (var route in outputs)
                    {
                        var worker = new OutputWorker(enumerator.GetDevice(route.DeviceId), route.Mode, sourceFormat);
                        worker.Start();
                        _outputs.Add(worker);
                    }
                }

                _capture.DataAvailable += (s, e) =>
                {
                    try
                    {
                        if (!_running || e.BytesRecorded <= 0) return;
                        var input = new byte[e.BytesRecorded];
                        Buffer.BlockCopy(e.Buffer, 0, input, 0, e.BytesRecorded);

                        lock (_gate)
                        {
                            foreach (var worker in _outputs)
                            {
                                var routed = RouteAudio(input, e.BytesRecorded, sourceFormat, sourceMode, worker.Mode);
                                worker.AddSamples(routed, routed.Length);
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        SetError(ex.Message);
                    }
                };

                _capture.RecordingStopped += (s, e) =>
                {
                    if (e.Exception != null) SetError(e.Exception.Message);
                    _running = false;
                };

                _running = true;
                _capture.StartRecording();
                enumerator.Dispose();

                error = "";
                return true;
            }
            catch (Exception ex)
            {
                SetError(ex.Message);
                Stop();
                error = ex.Message;
                return false;
            }
        }

        public void Stop()
        {
            _running = false;

            try
            {
                if (_capture != null)
                {
                    try { _capture.StopRecording(); } catch { }
                    _capture.Dispose();
                    _capture = null;
                }
            }
            catch { }

            lock (_gate)
            {
                foreach (var output in _outputs)
                {
                    try { output.Dispose(); } catch { }
                }
                _outputs.Clear();
            }
        }

        private void SetError(string message)
        {
            lock (_gate) _lastError = message ?? "";
        }

        public void Dispose() => Stop();

        private static byte[] RouteAudio(byte[] input, int bytesRecorded, WaveFormat format,
            ChannelMode sourceMode, ChannelMode outputMode)
        {
            int channels = Math.Max(1, format.Channels);
            int bits = format.BitsPerSample;
            int bytesPerSample = bits / 8;
            int blockAlign = format.BlockAlign;

            if (channels < 1 || bytesPerSample <= 0 || blockAlign <= 0 ||
                (bits != 16 && bits != 24 && bits != 32))
            {
                var passthrough = new byte[bytesRecorded];
                Buffer.BlockCopy(input, 0, passthrough, 0, bytesRecorded);
                return passthrough;
            }

            bool isFloat = IsFloat(format);
            var output = new byte[bytesRecorded];
            int frames = bytesRecorded / blockAlign;

            for (int frame = 0; frame < frames; frame++)
            {
                int offset = frame * blockAlign;
                float left = ReadSample(input, offset, bits, isFloat);
                float right = channels >= 2
                    ? ReadSample(input, offset + bytesPerSample, bits, isFloat)
                    : left;

                float outLeft = 0f;
                float outRight = 0f;

                if (sourceMode == ChannelMode.Stereo)
                {
                    if (outputMode == ChannelMode.Stereo)
                    {
                        outLeft = left;
                        outRight = right;
                    }
                    else
                    {
                        float mono = (left + right) * 0.5f;
                        if (outputMode == ChannelMode.Left) outLeft = mono;
                        else outRight = mono;
                    }
                }
                else
                {
                    float selected = sourceMode == ChannelMode.Left ? left : right;
                    if (outputMode == ChannelMode.Stereo)
                    {
                        outLeft = selected;
                        outRight = selected;
                    }
                    else if (outputMode == ChannelMode.Left)
                    {
                        outLeft = selected;
                    }
                    else
                    {
                        outRight = selected;
                    }
                }

                WriteSample(output, offset, bits, isFloat, outLeft);
                if (channels >= 2)
                    WriteSample(output, offset + bytesPerSample, bits, isFloat, outRight);
            }

            return output;
        }

        private static bool IsFloat(WaveFormat format)
        {
            if (format.Encoding == WaveFormatEncoding.IeeeFloat) return true;
            var extensible = format as WaveFormatExtensible;
            if (extensible == null) return false;
            return extensible.SubFormat == new Guid("00000003-0000-0010-8000-00AA00389B71");
        }

        private static float ReadSample(byte[] data, int offset, int bits, bool isFloat)
        {
            if (bits == 32 && isFloat)
                return Clamp(BitConverter.ToSingle(data, offset));

            if (bits == 16)
                return BitConverter.ToInt16(data, offset) / 32768f;

            if (bits == 24)
            {
                int value = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
                if ((value & 0x800000) != 0) value |= unchecked((int)0xFF000000);
                return value / 8388608f;
            }

            if (bits == 32)
                return BitConverter.ToInt32(data, offset) / 2147483648f;

            return 0f;
        }

        private static void WriteSample(byte[] data, int offset, int bits, bool isFloat, float value)
        {
            value = Clamp(value);

            if (bits == 32 && isFloat)
            {
                var bytes = BitConverter.GetBytes(value);
                Buffer.BlockCopy(bytes, 0, data, offset, 4);
                return;
            }

            if (bits == 16)
            {
                var bytes = BitConverter.GetBytes((short)(value * 32767f));
                Buffer.BlockCopy(bytes, 0, data, offset, 2);
                return;
            }

            if (bits == 24)
            {
                int sample = (int)(value * 8388607f);
                data[offset] = (byte)sample;
                data[offset + 1] = (byte)(sample >> 8);
                data[offset + 2] = (byte)(sample >> 16);
                return;
            }

            if (bits == 32)
            {
                var bytes = BitConverter.GetBytes((int)(value * 2147483647f));
                Buffer.BlockCopy(bytes, 0, data, offset, 4);
            }
        }

        private static float Clamp(float value)
        {
            if (value < -1f) return -1f;
            if (value > 1f) return 1f;
            return value;
        }

        private sealed class OutputWorker : IDisposable
        {
            private readonly MMDevice _device;
            private readonly BufferedWaveProvider _buffer;
            private readonly MediaFoundationResampler _resampler;
            private readonly WasapiOut _player;

            public ChannelMode Mode { get; }

            public OutputWorker(MMDevice device, ChannelMode mode, WaveFormat sourceFormat)
            {
                _device = device;
                Mode = mode;

                _buffer = new BufferedWaveProvider(sourceFormat)
                {
                    DiscardOnBufferOverflow = true,
                    BufferDuration = TimeSpan.FromMilliseconds(500)
                };

                var targetFormat = _device.AudioClient.MixFormat;
                _resampler = new MediaFoundationResampler(_buffer, targetFormat)
                {
                    ResamplerQuality = 60
                };

                _player = new WasapiOut(_device, AudioClientShareMode.Shared, true, 80);
                _player.Init(_resampler);
            }

            public void Start() => _player.Play();

            public void AddSamples(byte[] data, int count)
            {
                _buffer.AddSamples(data, 0, count);
            }

            public void Dispose()
            {
                try { _player.Stop(); } catch { }
                _player.Dispose();
                _resampler.Dispose();
                _device.Dispose();
            }
        }
    }
}
