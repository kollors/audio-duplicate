using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using NAudio.Wave;

namespace AudioDuplicate
{
    /// <summary>
    /// Endpoint-independent WASAPI process-loopback capture.
    /// Captures all system render streams except this process tree, so audio
    /// rendered by Audio Duplicate itself is not fed back into the capture.
    /// Requires Windows 10 build 20348+ (Windows 11 is supported).
    /// </summary>
    internal sealed class ProcessLoopbackCapture : IDisposable
    {
        private const string ProcessLoopbackDevice = "VAD\\Process_Loopback";
        private const ushort VariantBlob = 65; // VT_BLOB
        private const int SilentFlag = 0x2;
        private const uint WaitObject0 = 0;
        private const uint WaitTimeout = 258;

        private static readonly Guid AudioClientInterfaceId =
            new Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");
        private static readonly Guid AudioCaptureClientInterfaceId =
            new Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317");

        private IAudioClient _audioClient;
        private IAudioCaptureClient _captureClient;
        private IActivateAudioInterfaceAsyncOperation _activationOperation;
        private IntPtr _sampleEvent;
        private Thread _captureThread;
        private volatile bool _stopping;
        private volatile bool _recording;

        public WaveFormat WaveFormat { get; } = new WaveFormat(48000, 16, 2);
        public event EventHandler<WaveInEventArgs> DataAvailable;
        public event EventHandler<StoppedEventArgs> RecordingStopped;

        public void StartRecording()
        {
            if (_recording)
                throw new InvalidOperationException("Process loopback capture is already running.");

            _stopping = false;
            ActivateClient();

            var nativeFormat = new WaveFormatEx
            {
                FormatTag = 1, // WAVE_FORMAT_PCM
                Channels = 2,
                SamplesPerSec = 48000,
                BitsPerSample = 16,
                BlockAlign = 4,
                AvgBytesPerSec = 192000,
                ExtraSize = 0
            };

            IntPtr formatPtr = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WaveFormatEx)));
            try
            {
                Marshal.StructureToPtr(nativeFormat, formatPtr, false);

                var flags = AudioClientStreamFlags.Loopback |
                            AudioClientStreamFlags.EventCallback |
                            AudioClientStreamFlags.AutoConvertPcm |
                            AudioClientStreamFlags.SrcDefaultQuality;

                int hr = _audioClient.Initialize(
                    AudioClientShareMode.Shared,
                    flags,
                    0,
                    0,
                    formatPtr,
                    IntPtr.Zero);
                Marshal.ThrowExceptionForHR(hr);
            }
            finally
            {
                Marshal.FreeHGlobal(formatPtr);
            }

            _sampleEvent = CreateEvent(IntPtr.Zero, false, false, null);
            if (_sampleEvent == IntPtr.Zero)
                throw new InvalidOperationException("Не удалось создать событие process-loopback.");

            Marshal.ThrowExceptionForHR(_audioClient.SetEventHandle(_sampleEvent));

            object captureObject;
            Guid iidCapture = AudioCaptureClientInterfaceId;
            Marshal.ThrowExceptionForHR(_audioClient.GetService(ref iidCapture, out captureObject));
            _captureClient = (IAudioCaptureClient)captureObject;

            Marshal.ThrowExceptionForHR(_audioClient.Start());
            _recording = true;

            _captureThread = new Thread(CaptureThread)
            {
                IsBackground = true,
                Name = "AudioDuplicate Process Loopback"
            };
            _captureThread.Start();
        }

        public void StopRecording()
        {
            if (!_recording && _captureThread == null)
                return;

            _stopping = true;
            if (_sampleEvent != IntPtr.Zero)
                SetEvent(_sampleEvent);

            if (_captureThread != null &&
                _captureThread.IsAlive &&
                Thread.CurrentThread != _captureThread)
            {
                _captureThread.Join(1500);
            }

            _captureThread = null;

            try { _audioClient?.Stop(); } catch { }
            try { _audioClient?.Reset(); } catch { }
            _recording = false;
        }

        private void ActivateClient()
        {
            var parameters = new ActivationParameters
            {
                ActivationType = ActivationType.ProcessLoopback,
                Process = new ProcessParameters
                {
                    ProcessId = unchecked((uint)Process.GetCurrentProcess().Id),
                    Mode = ProcessLoopbackMode.ExcludeProcessTree
                }
            };

            IntPtr parametersMemory = IntPtr.Zero;
            IntPtr variantMemory = IntPtr.Zero;
            var completion = new ActivationCompletion();

            try
            {
                parametersMemory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(ActivationParameters)));
                Marshal.StructureToPtr(parameters, parametersMemory, false);

                var variant = new BlobVariant
                {
                    Type = VariantBlob,
                    Size = Marshal.SizeOf(typeof(ActivationParameters)),
                    Data = parametersMemory
                };

                variantMemory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(BlobVariant)));
                Marshal.StructureToPtr(variant, variantMemory, false);

                Guid iid = AudioClientInterfaceId;
                int hr = ActivateAudioInterfaceAsync(
                    ProcessLoopbackDevice,
                    ref iid,
                    variantMemory,
                    completion,
                    out _activationOperation);
                Marshal.ThrowExceptionForHR(hr);

                if (!completion.Wait(TimeSpan.FromSeconds(5)))
                    throw new TimeoutException("Windows не ответила на запрос process-loopback.");

                _audioClient = completion.GetAudioClient();
            }
            finally
            {
                completion.Dispose();
                if (variantMemory != IntPtr.Zero) Marshal.FreeHGlobal(variantMemory);
                if (parametersMemory != IntPtr.Zero) Marshal.FreeHGlobal(parametersMemory);
            }
        }

        private void CaptureThread()
        {
            Exception stoppedException = null;

            try
            {
                while (!_stopping)
                {
                    uint wait = WaitForSingleObject(_sampleEvent, 250);
                    if (_stopping) break;
                    if (wait == WaitTimeout) continue;
                    if (wait != WaitObject0)
                        throw new InvalidOperationException("Ошибка ожидания process-loopback.");

                    DrainPackets();
                }
            }
            catch (Exception ex)
            {
                stoppedException = ex;
            }
            finally
            {
                _recording = false;
                try { RecordingStopped?.Invoke(this, new StoppedEventArgs(stoppedException)); } catch { }
            }
        }

        private void DrainPackets()
        {
            while (!_stopping)
            {
                uint nextFrames;
                Marshal.ThrowExceptionForHR(_captureClient.GetNextPacketSize(out nextFrames));
                if (nextFrames == 0) break;

                IntPtr data;
                uint frames;
                uint flags;
                ulong devicePosition;
                ulong qpcPosition;

                Marshal.ThrowExceptionForHR(_captureClient.GetBuffer(
                    out data,
                    out frames,
                    out flags,
                    out devicePosition,
                    out qpcPosition));

                try
                {
                    int bytes = checked((int)(frames * 4)); // 16-bit stereo
                    var managed = new byte[bytes];

                    if ((flags & SilentFlag) == 0 && data != IntPtr.Zero)
                        Marshal.Copy(data, managed, 0, bytes);

                    DataAvailable?.Invoke(this, new WaveInEventArgs(managed, bytes));
                }
                finally
                {
                    _captureClient.ReleaseBuffer(frames);
                }
            }
        }

        public void Dispose()
        {
            StopRecording();

            if (_sampleEvent != IntPtr.Zero)
            {
                CloseHandle(_sampleEvent);
                _sampleEvent = IntPtr.Zero;
            }

            ReleaseCom(ref _captureClient);
            ReleaseCom(ref _audioClient);
            ReleaseCom(ref _activationOperation);
        }

        private static void ReleaseCom<T>(ref T value) where T : class
        {
            var current = value;
            value = null;
            if (current != null && Marshal.IsComObject(current))
            {
                try { Marshal.ReleaseComObject(current); } catch { }
            }
        }

        [DllImport("Mmdevapi.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int ActivateAudioInterfaceAsync(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceInterfacePath,
            ref Guid interfaceId,
            IntPtr activationParameters,
            IActivateAudioInterfaceCompletionHandler completionHandler,
            out IActivateAudioInterfaceAsyncOperation activationOperation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateEvent(
            IntPtr eventAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool manualReset,
            [MarshalAs(UnmanagedType.Bool)] bool initialState,
            string name);

        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetEvent(IntPtr handle);

        [DllImport("kernel32.dll")]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        [ComVisible(true)]
        [Guid("41D949AB-9862-444A-80F6-C261334DA5EB")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IActivateAudioInterfaceCompletionHandler
        {
            [PreserveSig]
            int ActivateCompleted(IActivateAudioInterfaceAsyncOperation operation);
        }

        [ComVisible(true)]
        [Guid("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAgileObject
        {
        }

        [ComImport]
        [Guid("72A22D78-CDE4-431D-B8CC-843A71199B6D")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IActivateAudioInterfaceAsyncOperation
        {
            [PreserveSig]
            int GetActivateResult(
                out int activationResult,
                [MarshalAs(UnmanagedType.IUnknown)] out object activatedObject);
        }

        [ComImport]
        [Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioClient
        {
            [PreserveSig]
            int Initialize(
                AudioClientShareMode shareMode,
                AudioClientStreamFlags streamFlags,
                long bufferDuration,
                long periodicity,
                IntPtr format,
                IntPtr audioSessionGuid);

            [PreserveSig] int GetBufferSize(out uint bufferFrames);
            [PreserveSig] int GetStreamLatency(out long latency);
            [PreserveSig] int GetCurrentPadding(out uint paddingFrames);
            [PreserveSig] int IsFormatSupported(AudioClientShareMode shareMode, IntPtr format, out IntPtr closestMatch);
            [PreserveSig] int GetMixFormat(out IntPtr deviceFormat);
            [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
            [PreserveSig] int Start();
            [PreserveSig] int Stop();
            [PreserveSig] int Reset();
            [PreserveSig] int SetEventHandle(IntPtr eventHandle);
            [PreserveSig] int GetService(ref Guid serviceId, [MarshalAs(UnmanagedType.IUnknown)] out object service);
        }

        [ComImport]
        [Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioCaptureClient
        {
            [PreserveSig]
            int GetBuffer(
                out IntPtr data,
                out uint framesToRead,
                out uint flags,
                out ulong devicePosition,
                out ulong qpcPosition);

            [PreserveSig] int ReleaseBuffer(uint framesRead);
            [PreserveSig] int GetNextPacketSize(out uint framesInNextPacket);
        }

        [ComVisible(true)]
        [ClassInterface(ClassInterfaceType.None)]
        private sealed class ActivationCompletion :
            IActivateAudioInterfaceCompletionHandler,
            IAgileObject,
            IDisposable
        {
            private readonly ManualResetEventSlim _completed = new ManualResetEventSlim(false);
            private int _activationResult = unchecked((int)0x8000FFFF);
            private IAudioClient _audioClient;
            private Exception _error;

            public int ActivateCompleted(IActivateAudioInterfaceAsyncOperation operation)
            {
                try
                {
                    object activated;
                    int callResult = operation.GetActivateResult(out _activationResult, out activated);
                    if (callResult < 0) _activationResult = callResult;

                    if (_activationResult >= 0)
                        _audioClient = activated as IAudioClient;
                }
                catch (Exception ex)
                {
                    _error = ex;
                }
                finally
                {
                    _completed.Set();
                }

                return 0;
            }

            public bool Wait(TimeSpan timeout) => _completed.Wait(timeout);

            public IAudioClient GetAudioClient()
            {
                if (_error != null) throw _error;
                Marshal.ThrowExceptionForHR(_activationResult);

                if (_audioClient == null)
                    throw new InvalidOperationException("Windows не вернула process-loopback AudioClient.");

                return _audioClient;
            }

            public void Dispose() => _completed.Dispose();
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BlobVariant
        {
            public ushort Type;
            public ushort Reserved1;
            public ushort Reserved2;
            public ushort Reserved3;
            public int Size;
            public IntPtr Data;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ActivationParameters
        {
            public ActivationType ActivationType;
            public ProcessParameters Process;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessParameters
        {
            public uint ProcessId;
            public ProcessLoopbackMode Mode;
        }

        private enum ActivationType
        {
            Default = 0,
            ProcessLoopback = 1
        }

        private enum ProcessLoopbackMode
        {
            IncludeProcessTree = 0,
            ExcludeProcessTree = 1
        }

        private enum AudioClientShareMode
        {
            Shared = 0,
            Exclusive = 1
        }

        [Flags]
        private enum AudioClientStreamFlags : uint
        {
            Loopback = 0x00020000,
            EventCallback = 0x00040000,
            SrcDefaultQuality = 0x08000000,
            AutoConvertPcm = 0x80000000
        }

        [StructLayout(LayoutKind.Sequential, Pack = 2)]
        private struct WaveFormatEx
        {
            public ushort FormatTag;
            public ushort Channels;
            public uint SamplesPerSec;
            public uint AvgBytesPerSec;
            public ushort BlockAlign;
            public ushort BitsPerSample;
            public ushort ExtraSize;
        }
    }
}
