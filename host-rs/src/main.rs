use std::io::{self, IsTerminal, Write};
use std::os::fd::AsFd;
use std::process::ExitCode;
use std::thread;
use std::time::{Duration, Instant};

use knietty_host::bridge::{BridgeConfig, NetworkBridge};
use knietty_host::cli::{
    self, Action, ConnectOptions, DiagnoseOptions, DisplayMonitorOptions, LocalInputMode,
};
use knietty_host::control::invoke;
use knietty_host::diagnostics::{run_diagnostics, DiagnosticError, DiagnosticsConfig};
use knietty_host::discovery::{discover_network_devices, format_network_device};
use knietty_host::pty::{default_command, exit_status_code, PtySession};
use knietty_host::signals::ShutdownSignals;
use knietty_host::terminal_guard::LocalTerminalGuard;
use knietty_host::transport::SecurityMode;
use knietty_host::ui::{HostUi, Tone};
use nix::libc;

fn run_pty_smoke(
    command: &str,
    cols: u16,
    rows: u16,
    term: &str,
    timeout: Duration,
) -> Result<(), String> {
    let mut session = PtySession::spawn(command, cols, rows, term)
        .map_err(|error| format!("could not start PTY command: {error}"))?;
    if !session
        .process_group_is_isolated()
        .map_err(|error| format!("could not verify PTY process group: {error}"))?
    {
        return Err("PTY child did not enter an isolated process group".to_owned());
    }

    let deadline = Instant::now() + timeout;
    let mut stdout = io::stdout().lock();
    let mut buffer = [0_u8; 1024];
    let status = loop {
        loop {
            match session.read(&mut buffer) {
                Ok(0) => break,
                Ok(length) => stdout
                    .write_all(&buffer[..length])
                    .map_err(|error| format!("could not write PTY output: {error}"))?,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.raw_os_error() == Some(libc::EIO) => break,
                Err(error) => return Err(format!("could not read PTY output: {error}")),
            }
        }
        if let Some(status) = session
            .try_wait()
            .map_err(|error| format!("could not inspect PTY child: {error}"))?
        {
            break status;
        }
        if Instant::now() >= deadline {
            return Err(format!(
                "PTY smoke command exceeded its {:.3}s timeout",
                timeout.as_secs_f64()
            ));
        }
        thread::sleep(Duration::from_millis(5));
    };
    stdout
        .flush()
        .map_err(|error| format!("could not flush PTY output: {error}"))?;
    session
        .close()
        .map_err(|error| format!("could not clean up PTY child: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "PTY smoke command exited with status {}",
            exit_status_code(status)
        ))
    }
}

fn run_connect(options: ConnectOptions) -> Result<i32, String> {
    let ui = HostUi::detect();
    let stdin = io::stdin();
    let stdin_is_terminal = stdin.is_terminal();
    let local_input_enabled = match options.local_input {
        LocalInputMode::Auto => stdin_is_terminal,
        LocalInputMode::Enabled if !stdin_is_terminal => {
            return Err("--local-input requires an interactive terminal".to_owned())
        }
        LocalInputMode::Enabled => true,
        LocalInputMode::Disabled => false,
    };
    let command = options.command.unwrap_or_else(default_command);
    let security = match options.security {
        SecurityMode::Tls => "TLS 1.3",
        SecurityMode::InsecurePlaintext => "plaintext",
    };
    if ui.is_terminal() {
        ui.banner();
        ui.emit(
            Tone::Detail,
            format_args!("{command} · {}×{} · {security}", options.cols, options.rows),
        );
        let target = if options.host == "auto" {
            "discovering a terminal on the local network".to_owned()
        } else {
            format!("connecting to {}:{}", options.host, options.port)
        };
        ui.emit(Tone::Activity, target);
    } else if options.verbose {
        ui.emit(
            Tone::Info,
            format_args!(
                "starting {command:?} at {}x{} using {security}",
                options.cols, options.rows
            ),
        );
    }
    if let Some(path) = &options.capture_output {
        ui.emit(
            Tone::Warning,
            format_args!(
                "capturing raw host-to-X4 PTY output to {} (private; screen contents and echoed input may be captured)",
                path.display()
            ),
        );
    }

    let mut session = PtySession::spawn(&command, options.cols, options.rows, &options.term)
        .map_err(|error| format!("could not start PTY command: {error}"))?;
    let signals = ShutdownSignals::install()
        .map_err(|error| format!("could not install shutdown signal handlers: {error}"))?;
    let local_terminal = if local_input_enabled {
        let terminal = LocalTerminalGuard::enable(stdin.as_fd())
            .map_err(|error| format!("could not enable local keyboard input: {error}"))?;
        ui.emit(
            Tone::Info,
            "keyboard active · Ctrl+\\ exits · Ctrl+C goes to the X4 session",
        );
        Some(terminal)
    } else {
        None
    };
    let config = BridgeConfig {
        host: options.host,
        port: options.port,
        retry_interval: options.retry_interval,
        discovery_timeout: options.discovery_timeout,
        approval_timeout: options.approval_timeout,
        max_bps: options.max_bps,
        capture_output: options.capture_output,
        capture_limit: options.capture_limit,
        reconnect: options.reconnect,
        protocol: options.protocol,
        verbose: options.verbose,
        security: options.security,
    };
    let bridge_result = NetworkBridge::new(
        &mut session,
        config,
        &signals,
        local_terminal.as_ref().map(LocalTerminalGuard::fd),
    )
    .and_then(|mut bridge| bridge.run())
    .map_err(|error| error.to_string());

    // Restore the caller's terminal before waiting for the PTY child to end.
    drop(local_terminal);
    let close_result = session
        .close()
        .map_err(|error| format!("could not clean up PTY child: {error}"));
    let code = bridge_result?;
    close_result?;
    if ui.is_terminal() && code == 0 {
        ui.emit(Tone::Success, "session closed cleanly");
    }
    Ok(code)
}

fn run_diagnose(options: DiagnoseOptions) -> Result<i32, String> {
    let signals = ShutdownSignals::install()
        .map_err(|error| format!("could not install shutdown signal handlers: {error}"))?;
    let config = DiagnosticsConfig {
        host: options.host,
        port: options.port,
        suite: options.suite,
        output: options.output,
        repetitions: options.repetitions,
        settle: options.settle,
        discovery_timeout: options.discovery_timeout,
        approval_timeout: options.approval_timeout,
        command_timeout: options.command_timeout,
        verbose: options.verbose,
        security: options.security,
    };
    match run_diagnostics(config, &signals) {
        Ok(code) => Ok(code),
        Err(DiagnosticError::Interrupted(signal)) => Ok(128 + signal),
        Err(error) => Err(error.to_string()),
    }
}

fn run_display_monitor(options: DisplayMonitorOptions) -> Result<i32, String> {
    let signals = ShutdownSignals::install()
        .map_err(|error| format!("could not install shutdown signal handlers: {error}"))?;
    let started_at = Instant::now();
    let mut sample = 0_usize;
    loop {
        if let Some(signal) = signals.received() {
            return Ok(128 + signal);
        }
        let mut result = invoke(
            knietty_host::control::DisplayCommand::Heap,
            options.device.as_deref(),
            options.timeout,
        )
        .map_err(|error| error.to_string())?;
        sample += 1;
        if let Some(object) = result.as_object_mut() {
            object.insert("sample".to_owned(), sample.into());
            object.insert(
                "elapsed_ms".to_owned(),
                u64::try_from(started_at.elapsed().as_millis())
                    .unwrap_or(u64::MAX)
                    .into(),
            );
        }
        println!(
            "{}",
            serde_json::to_string(&result)
                .map_err(|error| format!("could not encode heap monitor sample: {error}"))?
        );
        if options.count.is_some_and(|count| sample >= count) {
            return Ok(0);
        }
        let deadline = Instant::now() + options.interval;
        while Instant::now() < deadline {
            if let Some(signal) = signals.received() {
                return Ok(128 + signal);
            }
            thread::sleep(
                Duration::from_millis(50).min(deadline.saturating_duration_since(Instant::now())),
            );
        }
    }
}

fn run() -> Result<i32, String> {
    match cli::parse(std::env::args_os().skip(1)).map_err(|error| error.to_string())? {
        Action::Help => print!("{}", cli::HELP),
        Action::Version => println!("knietty {}", env!("CARGO_PKG_VERSION")),
        Action::List { timeout, port } => {
            println!("NAME\tADDRESS\tPORT\tID\tTLS");
            let devices = discover_network_devices(timeout, port)
                .map_err(|error| format!("network discovery failed: {error}"))?;
            for device in devices {
                println!("{}", format_network_device(&device));
            }
        }
        Action::PtySmoke {
            command,
            cols,
            rows,
            term,
            timeout,
        } => run_pty_smoke(&command, cols, rows, &term, timeout)?,
        Action::Connect(options) => return run_connect(options),
        Action::Diagnose(options) => return run_diagnose(options),
        Action::Display(options) => {
            let print_result = options.json
                || matches!(
                    options.command,
                    knietty_host::control::DisplayCommand::Status
                        | knietty_host::control::DisplayCommand::Metrics
                        | knietty_host::control::DisplayCommand::Heap
                );
            let result = invoke(options.command, options.device.as_deref(), options.timeout)
                .map_err(|error| error.to_string())?;
            if print_result {
                println!(
                    "{}",
                    serde_json::to_string_pretty(&result)
                        .map_err(|error| format!("could not encode display response: {error}"))?
                );
            }
        }
        Action::DisplayMonitor(options) => return run_display_monitor(options),
    }
    Ok(0)
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => ExitCode::from(code.clamp(0, u8::MAX as i32) as u8),
        Err(error) => {
            HostUi::detect().emit(Tone::Error, error);
            ExitCode::FAILURE
        }
    }
}
