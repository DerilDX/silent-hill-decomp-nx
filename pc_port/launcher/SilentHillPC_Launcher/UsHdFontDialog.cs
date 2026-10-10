using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Shown by the Mod Manager when the US HD font is enabled under a
    /// combination it was not cut for -- anything other than US English on a
    /// USA disc. That pack carries the 84 US cells and no accents, so under any
    /// other language the game falls back to the original font for the letters
    /// it lacks; the EU pack has all 126 and travels to every disc.
    ///
    /// Asked, not forced: the font is the user's choice, and the answer is a
    /// download they may already have or may not want. Same shape and keys as
    /// ResidentTexturesDialog so the two behave alike.
    /// </summary>
    internal sealed class UsHdFontDialog : Form
    {
        public enum Choice { NotNow, OpenPage, NeverAsk }

        public Choice Result { get; private set; }

        public UsHdFontDialog()
        {
            Result = Choice.NotNow;

            Text = Loc.T("EU HD font recommended");
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ShowInTaskbar = false;
            ClientSize = new Size(520, 168);

            var head = new Label
            {
                Location = new Point(14, 14),
                Size = new Size(492, 22),
                Font = new Font(SystemFonts.MessageBoxFont, FontStyle.Bold),
                Text = Loc.T("The US HD font is installed, but it only fits US English on a USA disc."),
            };

            var body = new Label
            {
                Location = new Point(14, 42),
                Size = new Size(492, 68),
                Text = Loc.T(
                    "Your current disc or language needs letters the US font does not have, " +
                    "so the game will fall back to the original font for them.\n\n" +
                    "The EU HD font by ivanproff covers every language. Open its download page?"),
            };

            var yes = new Button
            {
                Text = Loc.T("&Yes, open the download page"),
                Location = new Point(14, 124),
                Size = new Size(190, 30),
                DialogResult = DialogResult.OK,
            };
            yes.Click += (s, e) => { Result = Choice.OpenPage; };

            var no = new Button
            {
                Text = Loc.T("&No, not now"),
                Location = new Point(212, 124),
                Size = new Size(110, 30),
                DialogResult = DialogResult.OK,
            };
            no.Click += (s, e) => { Result = Choice.NotNow; };

            var never = new Button
            {
                Text = Loc.T("&Don't ask again"),
                Location = new Point(346, 124),
                Size = new Size(160, 30),
                DialogResult = DialogResult.OK,
            };
            never.Click += (s, e) => { Result = Choice.NeverAsk; };

            // Sized for the English; translations grow the head, the body and the buttons.
            int headH = TextRenderer.MeasureText(head.Text, head.Font, new Size(head.Width, 0), TextFormatFlags.WordBreak).Height;
            int headGrow = Math.Max(0, headH - head.Height);
            head.Height += headGrow;
            body.Top += headGrow;

            int bodyH = TextRenderer.MeasureText(body.Text, Font, new Size(body.Width, 0), TextFormatFlags.WordBreak).Height;
            int grow = headGrow + Math.Max(0, bodyH - body.Height);
            body.Height += grow - headGrow;
            foreach (var b in new[] { yes, no, never })
            {
                b.Top += grow;
                b.Width = Math.Max(b.Width, TextRenderer.MeasureText(b.Text, Font).Width + 18);
            }
            no.Left = yes.Right + 8;
            never.Left = Math.Max(no.Right + 8, ClientSize.Width - 14 - never.Width);
            ClientSize = new Size(Math.Max(ClientSize.Width, never.Right + 14), ClientSize.Height + grow);

            Controls.Add(head);
            Controls.Add(body);
            Controls.Add(yes);
            Controls.Add(no);
            Controls.Add(never);
            AcceptButton = yes;
            // Esc or the X is "not now": dismissing a question is not answering it.
            CancelButton = no;
        }
    }
}
