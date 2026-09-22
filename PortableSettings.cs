using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AudioDuplicate
{
    internal sealed class AppSettings
    {
        public bool SaveBesideExe { get; set; }
        public bool AutoRefreshDevices { get; set; } = true;
        public string Language { get; set; } = "ru";
        public string SourceDeviceId { get; set; } = "";
        public ChannelMode SourceMode { get; set; } = ChannelMode.Stereo;
        public List<OutputRoute> Outputs { get; } = new List<OutputRoute>();
    }

    internal static class PortableSettings
    {
        private static string PathName =>
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "AudioDuplicate.ini");

        public static AppSettings Load()
        {
            var s = new AppSettings();
            if (!File.Exists(PathName))
                return s;

            try
            {
                foreach (var raw in File.ReadAllLines(PathName, Encoding.UTF8))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line.StartsWith("#")) continue;
                    var p = line.IndexOf('=');
                    if (p <= 0) continue;
                    var key = line.Substring(0, p);
                    var value = line.Substring(p + 1);

                    switch (key)
                    {
                        case "save": s.SaveBesideExe = value == "1"; break;
                        case "autoRefresh": s.AutoRefreshDevices = value != "0"; break;
                        case "language": s.Language = value == "en" ? "en" : "ru"; break;
                        case "source": s.SourceDeviceId = Decode(value); break;
                        case "sourceMode":
                            if (int.TryParse(value, out var sm) && sm >= 0 && sm <= 2)
                                s.SourceMode = (ChannelMode)sm;
                            break;
                        case "output":
                            var parts = value.Split('|');
                            if (parts.Length == 2 && int.TryParse(parts[1], out var om) && om >= 0 && om <= 2)
                                s.Outputs.Add(new OutputRoute { DeviceId = Decode(parts[0]), Mode = (ChannelMode)om });
                            break;
                    }
                }
            }
            catch { }
            return s;
        }

        public static void Save(AppSettings s)
        {
            if (!s.SaveBesideExe)
            {
                try { if (File.Exists(PathName)) File.Delete(PathName); } catch { }
                return;
            }

            var lines = new List<string>
            {
                "save=1",
                "autoRefresh=" + (s.AutoRefreshDevices ? "1" : "0"),
                "language=" + (s.Language == "en" ? "en" : "ru"),
                "source=" + Encode(s.SourceDeviceId),
                "sourceMode=" + (int)s.SourceMode
            };
            lines.AddRange(s.Outputs.Select(o => "output=" + Encode(o.DeviceId) + "|" + (int)o.Mode));
            File.WriteAllLines(PathName, lines, new UTF8Encoding(false));
        }

        private static string Encode(string s) =>
            Convert.ToBase64String(Encoding.UTF8.GetBytes(s ?? ""));

        private static string Decode(string s)
        {
            try { return Encoding.UTF8.GetString(Convert.FromBase64String(s)); }
            catch { return ""; }
        }
    }
}
