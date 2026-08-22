use std::env;
use std::fmt;
use std::io::{self, IsTerminal, Write};
use std::os::fd::AsFd;

use nix::sys::termios::{self, OutputFlags};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Tone {
    Info,
    Activity,
    Success,
    Warning,
    Error,
    Detail,
}

#[derive(Clone, Debug)]
pub struct HostUi {
    terminal: bool,
    color: bool,
}

impl HostUi {
    pub fn detect() -> Self {
        let terminal = io::stderr().is_terminal();
        let color = terminal
            && env::var_os("NO_COLOR").is_none()
            && match env::var_os("TERM") {
                Some(term) => term != "dumb",
                None => true,
            };
        Self { terminal, color }
    }

    pub fn is_terminal(&self) -> bool {
        self.terminal
    }

    pub fn banner(&self) {
        if !self.terminal {
            return;
        }
        let line = if self.color {
            format!(
                "\x1b[1;36mknietty\x1b[0m  \x1b[2mv{} · wireless E Ink terminal\x1b[0m",
                env!("CARGO_PKG_VERSION")
            )
        } else {
            format!(
                "knietty  v{} · wireless E Ink terminal",
                env!("CARGO_PKG_VERSION")
            )
        };
        self.write_record(&line);
    }

    pub fn emit(&self, tone: Tone, message: impl fmt::Display) {
        self.write_record(&self.render(tone, &message.to_string()));
    }

    pub fn pairing(&self, device: &str, code: &str) {
        if !self.terminal {
            self.emit(
                Tone::Info,
                format_args!(
                    "first pairing code {code} for {device} (verify it matches the X4 before pressing Confirm)"
                ),
            );
            return;
        }

        let record = if self.color {
            format!(
                "\x1b[2mknietty\x1b[0m  \x1b[36m╭─\x1b[0m first-time pairing · {device}\n\x1b[2m         \x1b[0m\x1b[36m│\x1b[0m  code  \x1b[1;33m{code}\x1b[0m\n\x1b[2m         \x1b[0m\x1b[36m╰─\x1b[0m match it on the X4, then press Confirm"
            )
        } else {
            format!(
                "knietty  ╭─ first-time pairing · {device}\n         │  code  {code}\n         ╰─ match it on the X4, then press Confirm"
            )
        };
        self.write_record(&record);
    }

    fn render(&self, tone: Tone, message: &str) -> String {
        if !self.terminal {
            return format!("knietty: {message}");
        }

        let (glyph, color) = match tone {
            Tone::Info => ("›", "36"),
            Tone::Activity => ("◆", "36"),
            Tone::Success => ("✓", "32"),
            Tone::Warning => ("!", "33"),
            Tone::Error => ("×", "31"),
            Tone::Detail => ("·", "2"),
        };
        if self.color {
            format!("\x1b[2mknietty\x1b[0m  \x1b[{color}m{glyph}\x1b[0m {message}")
        } else {
            format!("knietty  {glyph} {message}")
        }
    }

    fn write_record(&self, record: &str) {
        let stderr = io::stderr();
        let output_postprocessing = termios::tcgetattr(stderr.as_fd())
            .map(|attributes| attributes.output_flags.contains(OutputFlags::OPOST))
            .unwrap_or(true);
        let ending = record_ending(self.terminal, output_postprocessing);
        let mut stderr = stderr.lock();
        for line in record.lines() {
            let _ = stderr.write_all(line.as_bytes());
            let _ = stderr.write_all(ending.as_bytes());
        }
        let _ = stderr.flush();
    }
}

fn record_ending(terminal: bool, output_postprocessing: bool) -> &'static str {
    if terminal && !output_postprocessing {
        "\r\n"
    } else {
        "\n"
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn redirected_output_stays_plain_and_script_friendly() {
        let ui = HostUi {
            terminal: false,
            color: false,
        };
        assert_eq!(ui.render(Tone::Success, "connected"), "knietty: connected");
    }

    #[test]
    fn interactive_no_color_output_keeps_semantic_marker() {
        let ui = HostUi {
            terminal: true,
            color: false,
        };
        assert_eq!(
            ui.render(Tone::Warning, "connection dropped"),
            "knietty  ! connection dropped"
        );
    }

    #[test]
    fn styled_output_resets_color_before_message() {
        let ui = HostUi {
            terminal: true,
            color: true,
        };
        assert_eq!(
            ui.render(Tone::Success, "connected"),
            "\x1b[2mknietty\x1b[0m  \x1b[32m✓\x1b[0m connected"
        );
    }

    #[test]
    fn raw_terminal_records_supply_the_carriage_return() {
        assert_eq!(record_ending(true, false), "\r\n");
        assert_eq!(record_ending(true, true), "\n");
        assert_eq!(record_ending(false, false), "\n");
    }
}
