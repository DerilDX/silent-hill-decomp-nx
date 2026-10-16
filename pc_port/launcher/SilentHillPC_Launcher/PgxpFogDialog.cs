using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Shown when the fog slider drops below 1.00 while PGXP is off. The
    /// distant town is depth-sorted on PGXP's per-vertex depth, so without it
    /// thinner fog only reveals empty sky.
    /// </summary>
    internal sealed class PgxpFogDialog : Form
    {
        public enum Choice { No, Yes, NeverAsk }

        public Choice Result { get; private set; }

        public PgxpFogDialog()
        {
            Result = Choice.No;

            Text = Loc.T("PGXP is turned off");
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ShowInTaskbar = false;
            ClientSize = new Size(440, 130);

            var body = new Label
            {
                Location = new Point(14, 14),
                Size = new Size(412, 60),
                Text = Loc.T(
                    "To see distant world geometry beyond the fog, PGXP needs to be on.\n\n" +
                    "Would you like to enable PGXP as well?"),
            };

            var yes = new Button { Text = Loc.T("Yes"), Location = new Point(14, 88), Size = new Size(90, 28), DialogResult = DialogResult.OK };
            yes.Click += (s, e) => { Result = Choice.Yes; };
            var no = new Button { Text = Loc.T("No"), Location = new Point(112, 88), Size = new Size(90, 28), DialogResult = DialogResult.OK };
            no.Click += (s, e) => { Result = Choice.No; };
            var never = new Button { Text = Loc.T("Do Not Ask Again"), Location = new Point(286, 88), Size = new Size(140, 28), DialogResult = DialogResult.OK };
            never.Click += (s, e) => { Result = Choice.NeverAsk; };

            // Sized for the English; translations grow the body and the buttons.
            int bodyH = TextRenderer.MeasureText(body.Text, Font, new Size(body.Width, 0), TextFormatFlags.WordBreak).Height;
            int grow = Math.Max(0, bodyH - body.Height);
            body.Height += grow;
            foreach (var b in new[] { yes, no, never })
            {
                b.Top += grow;
                b.Width = Math.Max(b.Width, TextRenderer.MeasureText(b.Text, Font).Width + 18);
            }
            no.Left = yes.Right + 8;
            never.Left = Math.Max(no.Right + 8, ClientSize.Width - 14 - never.Width);
            ClientSize = new Size(Math.Max(ClientSize.Width, never.Right + 14), ClientSize.Height + grow);

            Controls.Add(body);
            Controls.Add(yes);
            Controls.Add(no);
            Controls.Add(never);
            AcceptButton = yes;
            CancelButton = no;
        }
    }
}
