// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The editor launches a configured runtime after a confirmed save. It does not own the game's
//! input/network/weapon systems. Arguments are an argv vector; no shell parses user paths.
use std::fs::OpenOptions;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone, Debug)]
pub(super) struct Config {
    pub executable: PathBuf,
    pub args: Vec<String>,
}
impl Config {
    pub fn parse(args: &[String]) -> Result<Option<Self>, String> {
        let mut executable = None;
        let mut game_args = Vec::new();
        let mut i = 0;
        while i < args.len() {
            match args[i].as_str() {
                "--game" => {
                    i += 1;
                    let value = args
                        .get(i)
                        .filter(|v| !v.is_empty())
                        .ok_or("--game needs an executable")?;
                    if executable.replace(PathBuf::from(value)).is_some() {
                        return Err("--game supplied more than once".into());
                    }
                }
                "--game-arg" => {
                    i += 1;
                    let value = args.get(i).ok_or("--game-arg needs one argument")?;
                    if value == "--scene" || value.starts_with("--scene=") {
                        return Err("the editor supplies --scene from its confirmed save".into());
                    }
                    game_args.push(value.clone());
                }
                _ => {}
            }
            i += 1;
        }
        match executable {
            Some(executable) => Ok(Some(Self {
                executable,
                args: game_args,
            })),
            None if game_args.is_empty() => Ok(None),
            None => Err("--game-arg requires --game".into()),
        }
    }
}

#[derive(Default)]
pub(super) struct GameRun {
    pub config: Option<Config>,
    child: Option<Child>,
    pub status: Option<String>,
    pub log: Option<PathBuf>,
}
impl GameRun {
    pub fn running(&self) -> bool {
        self.child.is_some()
    }
    pub fn launch(&mut self, scene: &str) -> Result<(), String> {
        if self.running() {
            return Err("a game is already running".into());
        }
        let config = self
            .config
            .as_ref()
            .ok_or("no game configured; use the city editor launcher")?;
        let scene = absolute(Path::new(scene))?;
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos();
        let log =
            std::env::temp_dir().join(format!("rime-game-{}-{stamp}.log", std::process::id()));
        let file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&log)
            .map_err(|e| format!("game log: {e}"))?;
        let errors = file.try_clone().map_err(|e| e.to_string())?;
        let child = Command::new(&config.executable)
            .args(&config.args)
            .arg("--scene")
            .arg(&scene)
            .stdin(Stdio::null())
            .stdout(file)
            .stderr(errors)
            .spawn()
            .map_err(|e| format!("could not launch {}: {e}", config.executable.display()))?;
        self.status = Some(format!("Game running · pid {}", child.id()));
        self.log = Some(log);
        self.child = Some(child);
        Ok(())
    }
    pub fn poll(&mut self) {
        let Some(child) = &mut self.child else { return };
        match child.try_wait() {
            Ok(None) => {}
            Ok(Some(status)) => {
                self.status = Some(format!("Game exited: {status}"));
                self.child = None;
            }
            Err(e) => {
                self.status = Some(format!("Game status error: {e}"));
                self.child = None;
            }
        }
    }
}
// A launched game is independent: closing the editor does not kill it. Child::drop neither kills
// nor blocks, so an interactive runtime survives its tool's shutdown as documented in the UI.
fn absolute(path: &Path) -> Result<PathBuf, String> {
    if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        Ok(std::env::current_dir()
            .map_err(|e| e.to_string())?
            .join(path))
    }
}

pub(super) struct PendingSave {
    pub target: String,
    pub run: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    fn args(s: &[&str]) -> Vec<String> {
        s.iter().map(|v| v.to_string()).collect()
    }
    #[test]
    fn explicit_argv_and_scene_ownership() {
        let config = Config::parse(&args(&[
            "--game",
            "path with spaces/game",
            "--game-arg",
            "--play",
            "--game-arg",
            "quoted $() value",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(config.args, ["--play", "quoted $() value"]);
        assert!(Config::parse(&args(&["--game", "game", "--game-arg", "--scene"])).is_err());
        assert!(Config::parse(&args(&["--game-arg", "--play"])).is_err());
        assert!(Config::parse(&args(&["--game"])).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn missing_executable_fails_and_child_exit_is_reaped() {
        let mut run = GameRun {
            config: Some(Config {
                executable: "/does-not-exist/rime-game".into(),
                args: vec![],
            }),
            ..Default::default()
        };
        assert!(run.launch("/tmp/a scene.rscene").is_err());
        assert!(!run.running());
        run.config.as_mut().unwrap().executable = "/bin/echo".into();
        run.config.as_mut().unwrap().args = vec!["literal $(no-shell)".into()];
        run.launch("/tmp/a scene.rscene").unwrap();
        assert!(run.launch("/tmp/other.rscene").is_err());
        // A bounded wait is a process-lifecycle test, not a frame/performance measurement.
        for _ in 0..100 {
            run.poll();
            if !run.running() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(!run.running());
        let path = run.log.take().unwrap();
        let output = std::fs::read_to_string(&path).unwrap();
        assert!(output.contains("literal $(no-shell) --scene /tmp/a scene.rscene"));
        std::fs::remove_file(path).unwrap();
    }
}
