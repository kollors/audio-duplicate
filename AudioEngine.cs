using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

namespace AudioDuplicate
{
    internal sealed class AudioEngine : IDisposable
    {
        private Thread _captureThread;
        private volatile bool _stop;
        private volatile bool _running;
        private string _lastError = "";
        private readonly object _errorLock = new object();
        private readonly List<RendererWorker> _renderers = new List<RendererWorker>();

        public bool IsRunning => _running;
        public string LastError { get { lock (_errorLock) return _lastError; } }

        public static List<AudioDevice> EnumerateRenderDevices()
        {
            var result = new List<AudioDevice>();
            IMMDeviceEnumerator en = null;
            IMMDeviceCollection col = null;
            IMMDevice defaultDev = null;

            try
            {
                en = (IMMDeviceEnumerator)Activator.CreateInstance(
                    Type.GetTypeFromCLSID(CoreAudioIds.ClsidMmDeviceEnumerator));

                string defaultId = "";
                if (en.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eConsole, out defaultDev) >= 0 && defaultDev != null)
                    defaultDev.GetId(out defaultId);

                if (en.EnumAudioEndpoints(EDataFlow.eRender, DeviceState.Active, out col) < 0 || col == null)
                    return result;

                col.GetCount(out uint count);
                for (uint i = 0; i < count; i++)
                {
                    IMMDevice dev = null;
                    IPropertyStore store = null;
                    try
                    {
                        if (col.Item(i, out dev) < 0 || dev == null) continue;
                        dev.GetId(out string id);
                        string name = "Аудиоустройство";

                        if (dev.OpenPropertyStore(Native.STGM_READ, out store) >= 0 && store != null)
                        {
                            var key = CoreAudioIds.PkeyDeviceFriendlyName;
                            if (store.GetValue(ref key, out PropVariant pv) >= 0)
                            {
                                try
                                {
                                    if (pv.vt == 31 && pv.pointerValue != IntPtr.Zero)
                                        name = Marshal.PtrToStringUni(pv.pointerValue) ?? name;
                                }
                                finally { Native.PropVariantClear(ref pv); }
                            }
                        }

                        result.Add(new AudioDevice { Id = id, Name = name, IsDefault = id == defaultId });
                    }
                    finally
                    {
                        if (store != null) Marshal.ReleaseComObject(store);
                        if (dev != null) Marshal.ReleaseComObject(dev);
                    }
                }
            }
            catch { }
            finally
            {
                if (defaultDev != null) Marshal.ReleaseComObject(defaultDev);
                if (col != null) Marshal.ReleaseComObject(col);
                if (en != null) Marshal.ReleaseComObject(en);
            }

            return result.OrderByDescending(x => x.IsDefault)
                         .ThenBy(x => x.Name, StringComparer.CurrentCultureIgnoreCase)
                         .ToList();
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

            var ids = new HashSet<string>(StringComparer.Ordinal);
            foreach (var o in outputs)
            {
                if (string.IsNullOrWhiteSpace(o.DeviceId))
                {
                    error = "Выберите устройство для каждого дополнительного выхода.";
                    return false;
                }
                if (o.DeviceId == sourceDeviceId)
                {
                    error = "Основной и дополнительный выход не должны быть одним устройством.";
                    return false;
                }
                if (!ids.Add(o.DeviceId))
                {
                    error = "Одно дополнительное устройство выбрано несколько раз.";
                    return false;
                }
            }

            _stop = false;
            _running = true;

            var copy = outputs.Select(x => new OutputRoute { DeviceId = x.DeviceId, Mode = x.Mode }).ToList();
            _captureThread = new Thread(() => CaptureLoop(sourceDeviceId, sourceMode, copy))
            {
                IsBackground = true,
                Name = "AudioDuplicate Capture"
            };
            _captureThread.Start();
            error = "";
            return true;
        }

        public void Stop()
        {
            _stop = true;

            lock (_renderers)
            {
                foreach (var r in _renderers) r.Stop();
            }

            if (_captureThread != null && _captureThread.IsAlive)
                _captureThread.Join(2000);
            _captureThread = null;

            lock (_renderers)
            {
                foreach (var r in _renderers) r.Dispose();
                _renderers.Clear();
            }
            _running = false;
        }

        private void CaptureLoop(string sourceId, ChannelMode sourceMode, List<OutputRoute> outputs)
        {
            IMMDeviceEnumerator en = null;
            IMMDevice source = null;
            IAudioClient audioClient = null;
            IAudioCaptureClient capture = null;
            IntPtr mixPtr = IntPtr.Zero;
            IntPtr evt = IntPtr.Zero;

            try
            {
                en = (IMMDeviceEnumerator)Activator.CreateInstance(
                    Type.GetTypeFromCLSID(CoreAudioIds.ClsidMmDeviceEnumerator));

                if (en.GetDevice(sourceId, out source) < 0 || source == null)
                    throw new InvalidOperationException("Основное аудиоустройство недоступно.");

                var iidAudioClient = typeof(IAudioClient).GUID;
                if (source.Activate(ref iidAudioClient, Native.CLSCTX_ALL, IntPtr.Zero, out object clientObj) < 0)
                    throw new InvalidOperationException("Не удалось открыть основной выход.");
                audioClient = (IAudioClient)clientObj;

                if (audioClient.GetMixFormat(out mixPtr) < 0 || mixPtr == IntPtr.Zero)
                    throw new InvalidOperationException("Не удалось получить формат основного выхода.");

                var wf = Marshal.PtrToStructure<WaveFormatEx>(mixPtr);
                int formatBytes = Marshal.SizeOf<WaveFormatEx>() + wf.cbSize;
                var formatCopy = new byte[formatBytes];
                Marshal.Copy(mixPtr, formatCopy, 0, formatBytes);

                evt = Native.CreateEvent(IntPtr.Zero, false, false, null);
                if (evt == IntPtr.Zero)
                    throw new InvalidOperationException("Не удалось создать событие WASAPI.");

                var flags = AudioClientStreamFlags.Loopback | AudioClientStreamFlags.EventCallback;
                int hr = audioClient.Initialize(AudioClientShareMode.Shared, flags, 0, 0, mixPtr, IntPtr.Zero);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                hr = audioClient.SetEventHandle(evt);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                var iidCapture = typeof(IAudioCaptureClient).GUID;
                hr = audioClient.GetService(ref iidCapture, out object capObj);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                capture = (IAudioCaptureClient)capObj;

                lock (_renderers)
                {
                    foreach (var route in outputs)
                    {
                        var worker = new RendererWorker(route.DeviceId, route.Mode, formatCopy, wf);
                        if (!worker.Start(out string rendererError))
                            throw new InvalidOperationException(rendererError);
                        _renderers.Add(worker);
                    }
                }

                hr = audioClient.Start();
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                while (!_stop)
                {
                    if (Native.WaitForSingleObject(evt, 200) != Native.WAIT_OBJECT_0)
                        continue;

                    while (!_stop)
                    {
                        capture.GetNextPacketSize(out uint next);
                        if (next == 0) break;

                        int ghr = capture.GetBuffer(out IntPtr data, out uint frames,
                            out AudioClientBufferFlags packetFlags, out _, out _);
                        if (ghr < 0) Marshal.ThrowExceptionForHR(ghr);

                        try
                        {
                            bool silent = (packetFlags & AudioClientBufferFlags.Silent) != 0;
                            int bytes = checked((int)(frames * wf.nBlockAlign));
                            byte[] src = new byte[bytes];
                            if (!silent && data != IntPtr.Zero) Marshal.Copy(data, src, 0, bytes);

                            lock (_renderers)
                            {
                                foreach (var r in _renderers)
                                    r.Enqueue(RouteAudio(src, frames, wf, sourceMode, r.Mode, silent));
                            }
                        }
                        finally
                        {
                            capture.ReleaseBuffer(frames);
                        }
                    }
                }

                audioClient.Stop();
            }
            catch (Exception ex)
            {
                SetError(ex.Message);
            }
            finally
            {
                lock (_renderers)
                {
                    foreach (var r in _renderers) r.Stop();
                }

                if (evt != IntPtr.Zero) Native.CloseHandle(evt);
                if (mixPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(mixPtr);
                if (capture != null) Marshal.ReleaseComObject(capture);
                if (audioClient != null) Marshal.ReleaseComObject(audioClient);
                if (source != null) Marshal.ReleaseComObject(source);
                if (en != null) Marshal.ReleaseComObject(en);
                _running = false;
            }
        }

        private static byte[] RouteAudio(byte[] src, uint frames, WaveFormatEx wf,
            ChannelMode sourceMode, ChannelMode outputMode, bool silent)
        {
            int channels = wf.nChannels;
            int bits = wf.wBitsPerSample;
            int bytesPerSample = bits / 8;
            int frameBytes = wf.nBlockAlign;
            var output = new byte[checked((int)(frames * wf.nBlockAlign))];

            if (silent || src == null || channels <= 0 || bytesPerSample <= 0)
                return output;

            bool isFloat = wf.wFormatTag == 3 || (wf.wFormatTag == 0xFFFE && bits == 32);
            bool supported = (bits == 16 || bits == 24 || bits == 32);
            if (!supported)
            {
                Buffer.BlockCopy(src, 0, output, 0, Math.Min(src.Length, output.Length));
                return output;
            }

            for (int f = 0; f < frames; f++)
            {
                int off = f * frameBytes;
                float left = ReadSample(src, off, bits, isFloat);
                float right = channels >= 2 ? ReadSample(src, off + bytesPerSample, bits, isFloat) : left;

                float dl = 0, dr = 0;
                if (sourceMode == ChannelMode.Left || sourceMode == ChannelMode.Right)
                {
                    float selected = sourceMode == ChannelMode.Left ? left : right;
                    if (outputMode == ChannelMode.Stereo) dl = dr = selected;
                    else if (outputMode == ChannelMode.Left) dl = selected;
                    else dr = selected;
                }
                else
                {
                    if (outputMode == ChannelMode.Stereo) { dl = left; dr = right; }
                    else
                    {
                        float mono = (left + right) * 0.5f;
                        if (outputMode == ChannelMode.Left) dl = mono; else dr = mono;
                    }
                }

                WriteSample(output, off, bits, isFloat, dl);
                if (channels >= 2) WriteSample(output, off + bytesPerSample, bits, isFloat, dr);
            }
            return output;
        }

        private static float ReadSample(byte[] b, int o, int bits, bool isFloat)
        {
            if (bits == 32 && isFloat) return Math.Max(-1f, Math.Min(1f, BitConverter.ToSingle(b, o)));
            if (bits == 16) return BitConverter.ToInt16(b, o) / 32768f;
            if (bits == 24)
            {
                int v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);
                if ((v & 0x800000) != 0) v |= unchecked((int)0xFF000000);
                return v / 8388608f;
            }
            if (bits == 32) return BitConverter.ToInt32(b, o) / 2147483648f;
            return 0;
        }

        private static void WriteSample(byte[] b, int o, int bits, bool isFloat, float v)
        {
            v = Math.Max(-1f, Math.Min(1f, v));
            byte[] s;
            if (bits == 32 && isFloat) s = BitConverter.GetBytes(v);
            else if (bits == 16) s = BitConverter.GetBytes((short)(v * 32767f));
            else if (bits == 24)
            {
                int n = (int)(v * 8388607f);
                b[o] = (byte)n; b[o + 1] = (byte)(n >> 8); b[o + 2] = (byte)(n >> 16);
                return;
            }
            else if (bits == 32) s = BitConverter.GetBytes((int)(v * 2147483647f));
            else return;
            Buffer.BlockCopy(s, 0, b, o, s.Length);
        }

        private void SetError(string value) { lock (_errorLock) _lastError = value ?? ""; }

        public void Dispose() => Stop();

        private sealed class RendererWorker : IDisposable
        {
            private readonly string _deviceId;
            public ChannelMode Mode { get; }
            private readonly byte[] _formatBytes;
            private readonly WaveFormatEx _sourceFormat;
            private readonly BlockingCollection<byte[]> _queue =
                new BlockingCollection<byte[]>(new ConcurrentQueue<byte[]>(), 64);
            private Thread _thread;
            private volatile bool _stop;
            private readonly ManualResetEventSlim _init = new ManualResetEventSlim(false);
            private string _initError = "";
            private bool _initOk;

            public RendererWorker(string deviceId, ChannelMode mode, byte[] formatBytes, WaveFormatEx sourceFormat)
            {
                _deviceId = deviceId;
                Mode = mode;
                _formatBytes = formatBytes;
                _sourceFormat = sourceFormat;
            }

            public bool Start(out string error)
            {
                _thread = new Thread(Run) { IsBackground = true, Name = "AudioDuplicate Render" };
                _thread.Start();
                _init.Wait();
                error = _initError;
                return _initOk;
            }

            public void Enqueue(byte[] data)
            {
                if (_stop || data == null || data.Length == 0) return;
                while (!_queue.TryAdd(data))
                    _queue.TryTake(out _);
            }

            private void Run()
            {
                IMMDeviceEnumerator en = null;
                IMMDevice dev = null;
                IAudioClient client = null;
                IAudioRenderClient render = null;
                IntPtr fmt = IntPtr.Zero;
                IntPtr evt = IntPtr.Zero;

                try
                {
                    en = (IMMDeviceEnumerator)Activator.CreateInstance(
                        Type.GetTypeFromCLSID(CoreAudioIds.ClsidMmDeviceEnumerator));
                    if (en.GetDevice(_deviceId, out dev) < 0 || dev == null)
                        throw new InvalidOperationException("Не удалось открыть дополнительное аудиоустройство.");

                    var iidClient = typeof(IAudioClient).GUID;
                    int hr = dev.Activate(ref iidClient, Native.CLSCTX_ALL, IntPtr.Zero, out object obj);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                    client = (IAudioClient)obj;

                    fmt = Marshal.AllocCoTaskMem(_formatBytes.Length);
                    Marshal.Copy(_formatBytes, 0, fmt, _formatBytes.Length);

                    evt = Native.CreateEvent(IntPtr.Zero, false, false, null);
                    if (evt == IntPtr.Zero) throw new InvalidOperationException("Не удалось создать WASAPI event.");

                    var flags = AudioClientStreamFlags.EventCallback |
                                AudioClientStreamFlags.AutoConvertPcm |
                                AudioClientStreamFlags.SrcDefaultQuality;

                    hr = client.Initialize(AudioClientShareMode.Shared, flags, 0, 0, fmt, IntPtr.Zero);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                    hr = client.SetEventHandle(evt);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                    client.GetBufferSize(out uint bufferFrames);
                    var iidRender = typeof(IAudioRenderClient).GUID;
                    hr = client.GetService(ref iidRender, out object renderObj);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                    render = (IAudioRenderClient)renderObj;

                    hr = client.Start();
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);

                    _initOk = true;
                    _init.Set();

                    byte[] pending = null;
                    int offset = 0;
                    int frameBytes = _sourceFormat.nBlockAlign;

                    while (!_stop)
                    {
                        if (Native.WaitForSingleObject(evt, 200) != Native.WAIT_OBJECT_0)
                            continue;

                        if (client.GetCurrentPadding(out uint padding) < 0) continue;
                        uint available = bufferFrames > padding ? bufferFrames - padding : 0;
                        if (available == 0) continue;

                        if (render.GetBuffer(available, out IntPtr dst) < 0) continue;
                        int need = checked((int)(available * frameBytes));
                        var outBuf = new byte[need];
                        int written = 0;

                        while (written < need)
                        {
                            if (pending == null || offset >= pending.Length)
                            {
                                if (!_queue.TryTake(out pending)) break;
                                offset = 0;
                            }

                            int n = Math.Min(need - written, pending.Length - offset);
                            Buffer.BlockCopy(pending, offset, outBuf, written, n);
                            written += n;
                            offset += n;
                        }

                        Marshal.Copy(outBuf, 0, dst, outBuf.Length);
                        render.ReleaseBuffer(available, AudioClientBufferFlags.None);
                    }

                    client.Stop();
                }
                catch (Exception ex)
                {
                    if (!_init.IsSet)
                    {
                        _initError = ex.Message;
                        _initOk = false;
                        _init.Set();
                    }
                }
                finally
                {
                    if (!_init.IsSet) _init.Set();
                    if (evt != IntPtr.Zero) Native.CloseHandle(evt);
                    if (fmt != IntPtr.Zero) Marshal.FreeCoTaskMem(fmt);
                    if (render != null) Marshal.ReleaseComObject(render);
                    if (client != null) Marshal.ReleaseComObject(client);
                    if (dev != null) Marshal.ReleaseComObject(dev);
                    if (en != null) Marshal.ReleaseComObject(en);
                }
            }

            public void Stop()
            {
                _stop = true;
                if (_thread != null && _thread.IsAlive) _thread.Join(1500);
            }

            public void Dispose()
            {
                Stop();
                _queue.Dispose();
                _init.Dispose();
            }
        }
    }
}
