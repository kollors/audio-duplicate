using System;
using System.Drawing;
using System.Windows.Forms;

namespace AudioDuplicate
{
    internal sealed class SettingsForm : Form
    {
        private readonly CheckBox _autoRefresh;
        private readonly CheckBox _save;
        private readonly ComboBox _language;

        public bool AutoRefreshDevices => _autoRefresh.Checked;
        public bool SaveBesideExe => _save.Checked;
        public string Language => _language.SelectedIndex == 1 ? "en" : "ru";

        public SettingsForm(AppSettings settings)
        {
            Text = settings.Language == "en" ? "Settings" : "Настройки";
            ClientSize = new Size(410, 205);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;

            _autoRefresh = new CheckBox
            {
                Left = 22, Top = 24, Width = 350,
                Text = settings.Language == "en" ? "Auto-refresh device list" : "Автообновлять список устройств",
                Checked = settings.AutoRefreshDevices
            };
            _save = new CheckBox
            {
                Left = 22, Top = 62, Width = 350,
                Text = settings.Language == "en" ? "Save settings beside EXE" : "Сохранять настройки рядом с EXE",
                Checked = settings.SaveBesideExe
            };
            var label = new Label
            {
                Left = 22, Top = 108, Width = 90,
                Text = settings.Language == "en" ? "Language" : "Язык"
            };
            _language = new ComboBox
            {
                Left = 115, Top = 102, Width = 180,
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            _language.Items.AddRange(new object[] { "Русский", "English" });
            _language.SelectedIndex = settings.Language == "en" ? 1 : 0;

            var ok = new Button
            {
                Text = settings.Language == "en" ? "Save" : "Сохранить",
                Left = 198, Top = 155, Width = 90, DialogResult = DialogResult.OK
            };
            var cancel = new Button
            {
                Text = settings.Language == "en" ? "Cancel" : "Отмена",
                Left = 298, Top = 155, Width = 90, DialogResult = DialogResult.Cancel
            };

            Controls.AddRange(new Control[] { _autoRefresh, _save, label, _language, ok, cancel });
            AcceptButton = ok;
            CancelButton = cancel;
        }
    }
}
