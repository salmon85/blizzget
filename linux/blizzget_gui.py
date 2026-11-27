#!/usr/bin/env python3
"""Tkinter frontend for the bundled Cascette command-line downloader."""

from __future__ import annotations

import os
import queue
import re
import signal
import subprocess
import sys
import threading
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

DEFAULT_BUILD_CONFIG = "5194054cfa6480884c9a451b6f1107a3"
DEFAULT_CDN_CONFIG = "6547fc64ca796c7c1b25864145814ace"


def validate_inputs(build_config: str, cdn_config: str, output: str) -> str | None:
    """Return an error message for invalid input, otherwise None."""
    build_config = build_config.strip()
    cdn_config = cdn_config.strip()
    output = output.strip()
    if not re.fullmatch(r"[0-9a-fA-F]{32}", build_config):
        return "Build config must be a 32-character hexadecimal hash."
    if not re.fullmatch(r"[0-9a-fA-F]{32}", cdn_config):
        return "CDN config must be a 32-character hexadecimal hash."
    if not output:
        return "Choose an output directory."
    return None


def build_command(
    *,
    build_config: str,
    cdn_config: str,
    output: str,
    product: str,
    region: str,
    platform: str,
    arch: str,
    locale: str,
) -> list[str]:
    """Build the Cascette install command used by the GUI."""
    return [
        sys.executable,
        "-m",
        "cascette_tools",
        "install",
        "install-product",
        build_config.strip(),
        cdn_config.strip(),
        output.strip(),
        "--product",
        product,
        "--region",
        region,
        "--platform",
        platform,
        "--arch",
        arch,
        "--locale",
        locale,
        "--subfolder",
        "",
    ]


class BlizzGetGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("BlizzGet Linux")
        self.geometry("900x680")
        self.minsize(760, 560)

        self.process: subprocess.Popen[str] | None = None
        self.output_queue: queue.Queue[str | None] = queue.Queue()

        self.build_config = tk.StringVar(value=DEFAULT_BUILD_CONFIG)
        self.cdn_config = tk.StringVar(value=DEFAULT_CDN_CONFIG)
        self.output = tk.StringVar(
            value=str(Path.home() / "Games" / "Diablo III 2.7.4")
        )
        self.product = tk.StringVar(value="d3")
        self.region = tk.StringVar(value="us")
        self.platform = tk.StringVar(value="Windows")
        self.arch = tk.StringVar(value="x86_64")
        self.locale = tk.StringVar(value="enUS")
        self.status = tk.StringVar(value="Ready")

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        frame = ttk.Frame(self, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(8, weight=1)

        ttk.Label(frame, text="Build config").grid(row=0, column=0, sticky="w", pady=4)
        ttk.Entry(frame, textvariable=self.build_config).grid(
            row=0, column=1, columnspan=3, sticky="ew", pady=4
        )

        ttk.Label(frame, text="CDN config").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Entry(frame, textvariable=self.cdn_config).grid(
            row=1, column=1, columnspan=3, sticky="ew", pady=4
        )

        ttk.Label(frame, text="Output directory").grid(
            row=2, column=0, sticky="w", pady=4
        )
        ttk.Entry(frame, textvariable=self.output).grid(
            row=2, column=1, sticky="ew", pady=4
        )
        self.browse_button = ttk.Button(frame, text="Browse", command=self._browse)
        self.browse_button.grid(row=2, column=2, padx=(6, 0), pady=4)

        options = ttk.Frame(frame)
        options.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(8, 4))
        for column in range(8):
            options.columnconfigure(column, weight=1)

        self._add_option(
            options,
            0,
            "Product",
            self.product,
            [
                "d3",
                "wow",
                "wow_classic_era",
                "fenris",
                "pro",
                "hsb",
                "hero",
                "s2",
                "s1",
                "w3",
                "osi",
                "wlby",
                "agent",
                "bna",
                "bts",
                "catalogs",
            ],
        )
        self._add_option(
            options, 2, "Region", self.region, ["us", "eu", "kr", "tw", "cn"]
        )
        self._add_option(
            options, 4, "Platform", self.platform, ["Windows", "OSX", "Android", "iOS"]
        )
        self._add_option(
            options, 6, "Architecture", self.arch, ["x86_64", "x86_32", "arm64"]
        )
        self._add_option(
            options,
            8,
            "Locale",
            self.locale,
            [
                "enUS",
                "deDE",
                "esES",
                "esMX",
                "frFR",
                "koKR",
                "ptBR",
                "ruRU",
                "zhCN",
                "zhTW",
            ],
        )

        buttons = ttk.Frame(frame)
        buttons.grid(row=4, column=0, columnspan=4, sticky="ew", pady=8)
        self.download_button = ttk.Button(
            buttons, text="Download", command=self._start_download
        )
        self.download_button.pack(side="left")
        self.stop_button = ttk.Button(
            buttons, text="Stop", command=self._stop_download, state="disabled"
        )
        self.stop_button.pack(side="left", padx=(8, 0))
        ttk.Label(buttons, textvariable=self.status).pack(side="right")

        self.log = scrolledtext.ScrolledText(
            frame,
            wrap="word",
            height=20,
            state="disabled",
            font=("monospace", 9),
        )
        self.log.grid(row=8, column=0, columnspan=4, sticky="nsew", pady=(4, 0))

        ttk.Label(
            frame,
            text="Downloads official Blizzard CDN files. Ownership of the game is required.",
            foreground="#666666",
        ).grid(row=9, column=0, columnspan=4, sticky="w", pady=(6, 0))

        try:
            icon = tk.PhotoImage(file=str(Path(__file__).with_name("blizzget.png")))
            self.iconphoto(True, icon)
            self._icon = icon
        except tk.TclError:
            pass

    @staticmethod
    def _add_option(
        parent: ttk.Frame,
        column: int,
        label: str,
        variable: tk.StringVar,
        values: list[str],
    ) -> None:
        ttk.Label(parent, text=label).grid(
            row=0, column=column, sticky="w", padx=(0, 6)
        )
        ttk.Combobox(
            parent,
            textvariable=variable,
            values=values,
            state="readonly",
            width=14,
        ).grid(row=1, column=column, sticky="ew", padx=(0, 10))

    def _browse(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.output.get())
        if selected:
            self.output.set(selected)

    def _set_running(self, running: bool) -> None:
        for widget in (
            self.browse_button,
            self.download_button,
        ):
            widget.configure(state="disabled" if running else "normal")
        self.stop_button.configure(state="normal" if running else "disabled")

    def _start_download(self) -> None:
        error = validate_inputs(
            self.build_config.get(), self.cdn_config.get(), self.output.get()
        )
        if error:
            messagebox.showerror("Invalid configuration", error)
            return

        output = Path(self.output.get()).expanduser()
        if output.exists() and any(output.iterdir()):
            if not messagebox.askyesno(
                "Output directory is not empty",
                "The selected directory already contains files. Continue?",
            ):
                return
        output.mkdir(parents=True, exist_ok=True)
        self.output.set(str(output))

        command = build_command(
            build_config=self.build_config.get(),
            cdn_config=self.cdn_config.get(),
            output=str(output),
            product=self.product.get(),
            region=self.region.get(),
            platform=self.platform.get(),
            arch=self.arch.get(),
            locale=self.locale.get(),
        )
        env = os.environ.copy()
        env.setdefault("CASCETTE_MAX_CONCURRENCY", "48")
        env.setdefault("CASCETTE_MAX_PER_HOST", "12")

        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.status.set("Starting…")
        self._set_running(True)

        try:
            self.process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env,
            )
        except OSError as exc:
            self.process = None
            self._set_running(False)
            self.status.set("Failed to start")
            messagebox.showerror("Download failed", str(exc))
            return

        threading.Thread(target=self._read_output, daemon=True).start()
        self.after(100, self._drain_output)

    def _read_output(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            self.output_queue.put(None)
            return
        for line in process.stdout:
            self.output_queue.put(line.rstrip("\n"))
        self.output_queue.put(None)

    def _drain_output(self) -> None:
        while True:
            try:
                line = self.output_queue.get_nowait()
            except queue.Empty:
                break
            if line is None:
                self._finish_download()
                return
            self.log.configure(state="normal")
            self.log.insert("end", line + "\n")
            self.log.see("end")
            self.log.configure(state="disabled")

        process = self.process
        if process is not None:
            downloaded = sum(1 for _ in self.log.get("1.0", "end").splitlines())
            self.status.set(f"Running… ({downloaded} log lines)")
        self.after(100, self._drain_output)

    def _finish_download(self) -> None:
        process = self.process
        code = process.wait() if process is not None else -1
        self.process = None
        self._set_running(False)
        self.status.set("Completed" if code == 0 else f"Exited with code {code}")
        if code != 0:
            messagebox.showerror(
                "Download exited with an error",
                "See the log for details. Completed files are kept and the install can be resumed.",
            )

    def _stop_download(self) -> None:
        process = self.process
        if process is not None and process.poll() is None:
            self.status.set("Stopping…")
            process.send_signal(signal.SIGINT)
            self.stop_button.configure(state="disabled")

    def _on_close(self) -> None:
        process = self.process
        if process is not None and process.poll() is None:
            if not messagebox.askyesno(
                "Download in progress", "Stop the download and close?"
            ):
                return
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        self.destroy()


def main() -> None:
    BlizzGetGui().mainloop()


if __name__ == "__main__":
    main()
