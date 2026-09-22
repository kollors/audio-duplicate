using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace AudioDuplicate
{
    internal sealed class MainForm : Form
    {
        private readonly AudioEngine _engine = new AudioEngine();
        private readonly Timer _timer = new Timer();
        private AppSettings _settings;
        private List<AudioDevice> _devices = new List<AudioDevice>();

        private readonly ComboBox _sourceDevice = new ComboBox();
        private readonly ComboBox _sourceMode = new ComboBox();
        private readonly FlowLayoutPanel _outputsPanel = new FlowLayoutPanel();
        private readonly Label _sourceLabel = new Label();
        private readonly Label _outputsLabel = new Label();
        private readonly Label _status = new Label();
        private readonly Button _refresh = new Button();
        private readonly Button _settingsButton = new Button();
        private readonly Button _add = new Button();
        private readonly Button _start = new Button();

        private readonly List<OutputRow> _rows = new List<OutputRow>();

        public MainForm()
        {
            _settings = PortableSettings.Load();

            Text = "Audio Duplicate";
            MinimumSize = new Size(650, 480);
            Size = new Size(760, 610);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(18, 22, 27);
            ForeColor = Color.WhiteSmoke;
            Font = new Font("Segoe UI", 10F);

            _sourceLabel.AutoSize = true;
            _sourceLabel.Font = new Font(Font, FontStyle.Bold);
            _outputsLabel.AutoSize = true;
            _outputsLabel.Font = new Font(Font, FontStyle.Bold);

            ConfigureCombo(_sourceDevice);
            ConfigureCombo(_sourceMode);
            FillModes(_sourceMode, _settings.SourceMode);

            _refresh.Text = "↻";
            _settingsButton.Text = "⚙";
            _add.Text = "+";
            _start.Height = 38;

            _outputsPanel.AutoScroll = true;
            _outputsPanel.FlowDirection = FlowDirection.TopDown;
            _outputsPanel.WrapContents = false;
            _outputsPanel.BackColor = Color.FromArgb(25, 30, 37);

            Controls.AddRange(new Control[]
            {
                _sourceLabel, _sourceDevice, _sourceMode,
                _outputsLabel, _outputsPanel, _status,
                _refresh, _settingsButton, _add, _start
            });

            _refresh.Click += (_, __) => RefreshDevices();
            _settingsButton.Click += (_, __) => OpenSettings();
            _add.Click += (_, __) => AddOutput(null);
            _start.Click += (_, __) => ToggleAudio();

            Resize += (_, __) => LayoutUi();
            FormClosing += (_, __) =>
            {
                _engine.Stop();
                SaveCurrentSettings();
            };

            _timer.Interval = 3000;
            _timer.Tick += (_, __) =>
            {
                if (_engine.IsRunning)
                {
                    UpdateRunState();
                }
                else
                {
                    var err = _engine.LastError;
                    if (!string.IsNullOrWhiteSpace(err) && _start.Enabled)
                    {
                        UpdateRunState();
                        MessageBox.Show(this, err, "Audio Duplicate", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                    if (_settings.AutoRefreshDevices) RefreshDevices();
                }
            };

            RefreshDevices();
            if (_settings.Outputs.Count == 0) AddOutput(null);
            else foreach (var r in _settings.Outputs) AddOutput(r);

            ApplyLanguage();
            LayoutUi();
            _timer.Start();
        }

        private void ConfigureCombo(ComboBox c)
        {
            c.DropDownStyle = ComboBoxStyle.DropDownList;
            c.IntegralHeight = true;
            c.MaxDropDownItems = 12;
        }

        private void FillModes(ComboBox combo, ChannelMode selected)
        {
            combo.Items.Clear();
            combo.Items.AddRange(_settings.Language == "en"
                ? new object[] { "Stereo", "Left channel", "Right channel" }
                : new object[] { "Стерео", "Левый канал", "Правый канал" });
            combo.SelectedIndex = (int)selected;
        }

        private string SelectedDeviceId(ComboBox combo)
        {
            return (combo.SelectedItem as AudioDevice)?.Id ?? "";
        }

        private void SelectDevice(ComboBox combo, string id, bool preferDifferentFromSource)
        {
            combo.Items.Clear();
            foreach (var d in _devices) combo.Items.Add(d);

            var match = _devices.FirstOrDefault(d => d.Id == id);
            if (match != null) combo.SelectedItem = match;
            else if (preferDifferentFromSource)
            {
                var source = SelectedDeviceId(_sourceDevice);
                combo.SelectedItem = _devices.FirstOrDefault(d => d.Id != source);
            }
            else if (_devices.Count > 0)
                combo.SelectedIndex = 0;
        }

        private void RefreshDevices()
        {
            var sourceId = SelectedDeviceId(_sourceDevice);
            if (string.IsNullOrEmpty(sourceId)) sourceId = _settings.SourceDeviceId;
            var rowIds = _rows.Select(r => SelectedDeviceId(r.Device)).ToArray();

            _devices = AudioEngine.EnumerateRenderDevices();
            SelectDevice(_sourceDevice, sourceId, false);

            for (int i = 0; i < _rows.Count; i++)
                SelectDevice(_rows[i].Device, i < rowIds.Length ? rowIds[i] : "", true);
        }

        private void AddOutput(OutputRoute initial)
        {
            var row = new OutputRow();
            ConfigureCombo(row.Device);
            ConfigureCombo(row.Mode);
            FillModes(row.Mode, initial?.Mode ?? ChannelMode.Stereo);
            SelectDevice(row.Device, initial?.DeviceId ?? "", initial == null);

            row.Remove.Text = "−";
            row.Remove.Width = 42;
            row.Remove.Height = 30;
            row.Remove.Click += (_, __) =>
            {
                _outputsPanel.Controls.Remove(row.Panel);
                _rows.Remove(row);
                row.Panel.Dispose();
            };

            row.Panel.Height = 38;
            row.Panel.Margin = new Padding(0, 0, 0, 8);
            row.Panel.Controls.AddRange(new Control[] { row.Device, row.Mode, row.Remove });
            _rows.Add(row);
            _outputsPanel.Controls.Add(row.Panel);
            LayoutRows();
        }

        private void LayoutUi()
        {
            int w = ClientSize.Width;
            int h = ClientSize.Height;

            _refresh.SetBounds(w - 102, 12, 38, 34);
            _settingsButton.SetBounds(w - 56, 12, 38, 34);

            _sourceLabel.SetBounds(24, 76, 300, 24);
            _sourceDevice.SetBounds(24, 106, Math.Max(260, w - 240), 30);
            _sourceMode.SetBounds(w - 190, 106, 166, 30);

            _outputsLabel.SetBounds(24, 178, 300, 24);
            _add.SetBounds(w - 62, 166, 38, 34);
            _outputsPanel.SetBounds(24, 212, w - 48, Math.Max(120, h - 292));

            _status.SetBounds(24, h - 56, 310, 28);
            _start.SetBounds(w - 164, h - 66, 140, 38);
            LayoutRows();
        }

        private void LayoutRows()
        {
            int width = Math.Max(300, _outputsPanel.ClientSize.Width - 26);
            foreach (var r in _rows)
            {
                r.Panel.Width = width;
                int modeW = 150;
                int removeW = 42;
                int gap = 10;
                int deviceW = Math.Max(180, width - modeW - removeW - gap * 2);
                r.Device.SetBounds(0, 2, deviceW, 30);
                r.Mode.SetBounds(deviceW + gap, 2, modeW, 30);
                r.Remove.SetBounds(deviceW + gap + modeW + gap, 0, removeW, 32);
            }
        }

        private void ToggleAudio()
        {
            if (_engine.IsRunning)
            {
                _engine.Stop();
                UpdateRunState();
                return;
            }

            var routes = _rows.Select(r => new OutputRoute
            {
                DeviceId = SelectedDeviceId(r.Device),
                Mode = (ChannelMode)Math.Max(0, r.Mode.SelectedIndex)
            }).ToList();

            if (!_engine.Start(
                SelectedDeviceId(_sourceDevice),
                (ChannelMode)Math.Max(0, _sourceMode.SelectedIndex),
                routes,
                out string error))
            {
                MessageBox.Show(this, error, "Audio Duplicate", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            SaveCurrentSettings();
            UpdateRunState();
        }

        private void UpdateRunState()
        {
            bool running = _engine.IsRunning;
            _sourceDevice.Enabled = !running;
            _sourceMode.Enabled = !running;
            _refresh.Enabled = !running;
            _add.Enabled = !running;

            foreach (var r in _rows)
            {
                r.Device.Enabled = !running;
                r.Mode.Enabled = !running;
                r.Remove.Enabled = !running;
            }

            _status.Text = running
                ? (_settings.Language == "en" ? "●  Running" : "●  Работает")
                : (_settings.Language == "en" ? "●  Ready" : "●  Готов к работе");
            _start.Text = running
                ? (_settings.Language == "en" ? "Stop" : "Остановить")
                : (_settings.Language == "en" ? "Start" : "Запустить");
        }

        private void OpenSettings()
        {
            using (var dialog = new SettingsForm(_settings))
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;

                _settings.AutoRefreshDevices = dialog.AutoRefreshDevices;
                _settings.SaveBesideExe = dialog.SaveBesideExe;
                _settings.Language = dialog.Language;
                SaveCurrentSettings();
                ApplyLanguage();
            }
        }

        private void ApplyLanguage()
        {
            _sourceLabel.Text = _settings.Language == "en" ? "Main output" : "Основной выход";
            _outputsLabel.Text = _settings.Language == "en" ? "Additional outputs" : "Дополнительные выходы";

            var sm = (ChannelMode)Math.Max(0, _sourceMode.SelectedIndex);
            FillModes(_sourceMode, sm);
            foreach (var r in _rows)
            {
                var m = (ChannelMode)Math.Max(0, r.Mode.SelectedIndex);
                FillModes(r.Mode, m);
            }
            UpdateRunState();
        }

        private void SaveCurrentSettings()
        {
            _settings.SourceDeviceId = SelectedDeviceId(_sourceDevice);
            _settings.SourceMode = (ChannelMode)Math.Max(0, _sourceMode.SelectedIndex);
            _settings.Outputs.Clear();
            foreach (var r in _rows)
                _settings.Outputs.Add(new OutputRoute
                {
                    DeviceId = SelectedDeviceId(r.Device),
                    Mode = (ChannelMode)Math.Max(0, r.Mode.SelectedIndex)
                });
            PortableSettings.Save(_settings);
        }

        private sealed class OutputRow
        {
            public Panel Panel { get; } = new Panel();
            public ComboBox Device { get; } = new ComboBox();
            public ComboBox Mode { get; } = new ComboBox();
            public Button Remove { get; } = new Button();
        }
    }
}
