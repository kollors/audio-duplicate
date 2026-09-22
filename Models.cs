namespace AudioDuplicate
{
    internal enum ChannelMode { Stereo = 0, Left = 1, Right = 2 }

    internal sealed class AudioDevice
    {
        public string Id { get; set; }
        public string Name { get; set; }
        public bool IsDefault { get; set; }

        public override string ToString()
        {
            return Name + (IsDefault ? "  (по умолчанию)" : "");
        }
    }

    internal sealed class OutputRoute
    {
        public string DeviceId { get; set; } = "";
        public ChannelMode Mode { get; set; } = ChannelMode.Stereo;
    }
}
