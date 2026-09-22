using System;
using System.Runtime.InteropServices;

namespace AudioDuplicate
{
    internal enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    internal enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }

    [Flags]
    internal enum DeviceState : uint { Active = 0x1 }

    internal enum AudioClientShareMode { Shared = 0, Exclusive = 1 }

    [Flags]
    internal enum AudioClientStreamFlags : uint
    {
        CrossProcess = 0x00010000,
        Loopback = 0x00020000,
        EventCallback = 0x00040000,
        NoPersist = 0x00080000,
        RateAdjust = 0x00100000,
        AutoConvertPcm = 0x80000000,
        SrcDefaultQuality = 0x08000000
    }

    [Flags]
    internal enum AudioClientBufferFlags : uint
    {
        None = 0,
        DataDiscontinuity = 0x1,
        Silent = 0x2,
        TimestampError = 0x4
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct WaveFormatEx
    {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey
    {
        public Guid fmtid;
        public uint pid;
        public PropertyKey(Guid f, uint p) { fmtid = f; pid = p; }
    }

    [StructLayout(LayoutKind.Explicit)]
    internal struct PropVariant
    {
        [FieldOffset(0)] public ushort vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    internal static class CoreAudioIds
    {
        public static readonly Guid ClsidMmDeviceEnumerator = new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
        public static readonly PropertyKey PkeyDeviceFriendlyName =
            new PropertyKey(new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 14);
    }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState stateMask, out IMMDeviceCollection devices);
        int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        int RegisterEndpointNotificationCallback(IntPtr client);
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-C0A3D1C2D85E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        int GetCount(out uint count);
        int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        int Activate(ref Guid iid, uint clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object instance);
        int OpenPropertyStore(uint access, out IPropertyStore properties);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out DeviceState state);
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        int GetCount(out uint count);
        int GetAt(uint index, out PropertyKey key);
        int GetValue(ref PropertyKey key, out PropVariant value);
        int SetValue(ref PropertyKey key, ref PropVariant value);
        int Commit();
    }

    [ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioClient
    {
        int Initialize(AudioClientShareMode shareMode, AudioClientStreamFlags streamFlags,
            long bufferDuration, long periodicity, IntPtr format, IntPtr audioSessionGuid);
        int GetBufferSize(out uint bufferFrames);
        int GetStreamLatency(out long latency);
        int GetCurrentPadding(out uint paddingFrames);
        int IsFormatSupported(AudioClientShareMode shareMode, IntPtr format, out IntPtr closestMatch);
        int GetMixFormat(out IntPtr deviceFormat);
        int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
        int Start();
        int Stop();
        int Reset();
        int SetEventHandle(IntPtr eventHandle);
        int GetService(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
    }

    [ComImport, Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioCaptureClient
    {
        int GetBuffer(out IntPtr data, out uint frames, out AudioClientBufferFlags flags, out ulong devicePosition, out ulong qpcPosition);
        int ReleaseBuffer(uint frames);
        int GetNextPacketSize(out uint framesInNextPacket);
    }

    [ComImport, Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioRenderClient
    {
        int GetBuffer(uint framesRequested, out IntPtr data);
        int ReleaseBuffer(uint framesWritten, AudioClientBufferFlags flags);
    }

    internal static class Native
    {
        public const uint CLSCTX_ALL = 23;
        public const uint STGM_READ = 0;

        [DllImport("ole32.dll")]
        public static extern int PropVariantClear(ref PropVariant pvar);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr CreateEvent(IntPtr attributes, bool manualReset, bool initialState, string name);

        [DllImport("kernel32.dll")]
        public static extern bool SetEvent(IntPtr handle);

        [DllImport("kernel32.dll")]
        public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll")]
        public static extern bool CloseHandle(IntPtr handle);

        public const uint WAIT_OBJECT_0 = 0;
    }
}
