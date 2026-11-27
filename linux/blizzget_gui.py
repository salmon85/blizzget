#!/usr/bin/env python3
"""Tkinter frontend for the bundled Cascette command-line downloader."""

from __future__ import annotations

import json
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

DEFAULT_BUILD_CONFIG = "5194054cfa6480884c9a451b6f1107a3"
DEFAULT_CDN_CONFIG = "6547fc64ca796c7c1b25864145814ace"
VERSION_PRESETS: dict[str, tuple[str, str] | None] = {
    "Diablo III 2.6.10.73447": (
        "16da240fd5ef3c2be3290c0eca172b16",
        DEFAULT_CDN_CONFIG,
    ),
    "Diablo III 2.7.4.84161": (DEFAULT_BUILD_CONFIG, DEFAULT_CDN_CONFIG),
    "Diablo III 2.8.0.99920": (
        "e34fc3fb7831a77e0ad7def0bb399ac7",
        DEFAULT_CDN_CONFIG,
    ),
    "Custom": None,
}

PRODUCT_CODES = [
    "agent",
    "bna",
    "wow",
    "wow_classic",
    "wow_classic_era",
    "wow_classic_titan",
    "wow_anniversary",
    "wowt",
    "wowxptr",
    "d3",
    "fenris",
    "pro",
    "hsb",
    "hero",
    "s2",
    "s1",
    "w3",
    "osi",
    "wlby",
]
BLIZZTRACK_API = "https://blizztrack.com/api"


def parse_seqn_history(page: dict[str, Any]) -> list[int]:
    """Return unique snapshot sequence numbers from a BlizzTrack page."""
    values = []
    for item in page.get("results", []):
        try:
            value = int(item["seqn"])
        except (KeyError, TypeError, ValueError):
            continue
        if value not in values:
            values.append(value)
    return values


def parse_version_entries(
    entries: list[dict[str, Any]], region: str
) -> list[dict[str, str]]:
    """Convert BlizzTrack version rows into deduplicated build records."""
    records: list[dict[str, str]] = []
    seen: set[str] = set()
    for entry in entries:
        if entry.get("region") != region:
            continue
        build_config = str(entry.get("build_config", ""))
        if not build_config or build_config in seen:
            continue
        seen.add(build_config)
        records.append(
            {
                "version": str(
                    entry.get("version_name") or entry.get("build_id") or ""
                ),
                "build_config": build_config,
                "cdn_config": str(entry.get("cdn_config", "")),
                "product_config": str(entry.get("product_config", "")),
                "created_at": str(entry.get("created_at", "")),
            }
        )
    return records


def merge_version_records(
    groups: list[list[dict[str, str]]],
) -> list[dict[str, str]]:
    """Merge record groups while preserving the newest-first order."""
    merged: list[dict[str, str]] = []
    seen: set[str] = set()
    for group in groups:
        for record in group:
            build_config = record.get("build_config", "")
            if not build_config or build_config in seen:
                continue
            seen.add(build_config)
            merged.append(record)
    return merged


def _get_json(url: str) -> dict[str, Any]:
    last_error: Exception | None = None
    for attempt in range(3):
        try:
            request = urllib.request.Request(
                url,
                headers={
                    "User-Agent": "BlizzGet-Linux/1.1",
                    "Accept": "application/json",
                },
            )
            with urllib.request.urlopen(request, timeout=30) as response:
                return dict(json.load(response))
        except Exception as exc:  # noqa: BLE001 - surfaced to the GUI
            last_error = exc
            if attempt < 2:
                time.sleep(1 + attempt)
    raise RuntimeError(f"Request failed: {url}: {last_error}")


def _version_cache_path(product: str, region: str) -> Path:
    return Path.home() / ".cache" / "blizzget" / "versions" / product / f"{region}.json"


def fetch_version_history(product: str, region: str) -> list[dict[str, str]]:
    """Fetch all BlizzTrack version snapshots for one product and region."""
    cache_path = _version_cache_path(product, region)
    if cache_path.exists() and time.time() - cache_path.stat().st_mtime < 6 * 3600:
        try:
            cached = json.loads(cache_path.read_text(encoding="utf-8"))
            if isinstance(cached, list):
                return [dict(item) for item in cached]
        except (OSError, ValueError, TypeError):
            pass

    current = _get_json(f"{BLIZZTRACK_API}/manifest/{product}/versions")
    current_result = current.get("result") or {}
    groups = [parse_version_entries(list(current_result.get("data", [])), region)]

    first_page = _get_json(
        f"{BLIZZTRACK_API}/manifest/{product}/seqn" "?file=versions&page=1&limit=100"
    )
    first_result = first_page.get("result") or {}
    seqns = parse_seqn_history(first_result)
    total_pages = int(first_result.get("total_pages") or 1)

    def fetch_page(page: int) -> list[int]:
        if page == 1:
            return seqns
        data = _get_json(
            f"{BLIZZTRACK_API}/manifest/{product}/seqn"
            f"?file=versions&page={page}&limit=100"
        )
        return parse_seqn_history(data.get("result") or {})

    with ThreadPoolExecutor(max_workers=6) as executor:
        futures = [
            executor.submit(fetch_page, page) for page in range(2, total_pages + 1)
        ]
        for future in futures:
            try:
                for seqn in future.result():
                    if seqn not in seqns:
                        seqns.append(seqn)
            except Exception:
                continue

    def fetch_snapshot(seqn: int) -> list[dict[str, str]]:
        try:
            data = _get_json(
                f"{BLIZZTRACK_API}/manifest/{product}/versions?seqn={seqn}"
            )
            result = data.get("result") or {}
            records = parse_version_entries(list(result.get("data", [])), region)
            for record in records:
                record["created_at"] = str(result.get("created_at", ""))
            return records
        except Exception:
            return []

    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = [executor.submit(fetch_snapshot, seqn) for seqn in seqns]
        for future in futures:
            records = future.result()
            if records:
                groups.append(records)

    merged = merge_version_records(groups)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(merged, indent=2), encoding="utf-8")
    return merged


def version_label(record: dict[str, str]) -> str:
    """Create a readable label for a build record."""
    version = record.get("version") or "unknown"
    build_config = record.get("build_config", "")
    return f"{version} ({build_config[:8]})" if build_config else version


def validate_concurrency(global_value: str, per_host_value: str) -> str | None:
    """Validate GUI download connection limits."""
    try:
        global_limit = int(global_value)
        per_host_limit = int(per_host_value)
    except ValueError:
        return "Connection limits must be whole numbers."
    if global_limit <= 0 or per_host_limit <= 0:
        return "Connection limits must be positive numbers."
    if per_host_limit > global_limit:
        return "The per-host limit cannot exceed the global limit."
    return None


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
    runner = (
        [sys.executable, "--cli"]
        if getattr(sys, "frozen", False)
        else [sys.executable, "-m", "cascette_tools"]
    )
    return runner + [
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
        self.title("BlizzGet")
        self.geometry("900x680")
        self.minsize(760, 560)

        self.process: subprocess.Popen[str] | None = None
        self.output_queue: queue.Queue[str | None] = queue.Queue()
        self.version_queue: queue.Queue[tuple[Any, ...]] = queue.Queue()
        self.version_load_token = 0
        self.version_records: dict[str, dict[str, str]] = {
            label: {
                "version": label.removeprefix("Diablo III "),
                "build_config": preset[0],
                "cdn_config": preset[1],
            }
            for label, preset in VERSION_PRESETS.items()
            if preset is not None
        }

        self.version = tk.StringVar(value="Diablo III 2.7.4.84161")
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
        self.max_concurrency = tk.StringVar(
            value=os.getenv("CASCETTE_MAX_CONCURRENCY", "48")
        )
        self.max_per_host = tk.StringVar(value=os.getenv("CASCETTE_MAX_PER_HOST", "12"))
        self.status = tk.StringVar(value="Ready")

        self._build_ui()
        self.product.trace_add("write", self._on_product_or_region_changed)
        self.region.trace_add("write", self._on_product_or_region_changed)
        self.version.trace_add("write", self._on_version_changed)
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._reload_versions()

    def _build_ui(self) -> None:
        frame = ttk.Frame(self, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(9, weight=1)

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
        for column in range(12):
            options.columnconfigure(column, weight=1)

        self.version_combo = self._add_option(
            options,
            0,
            "Version",
            self.version,
            list(VERSION_PRESETS),
            width=24,
        )
        self.product_combo = self._add_option(
            options,
            2,
            "Product",
            self.product,
            PRODUCT_CODES,
        )
        self.region_combo = self._add_option(
            options, 4, "Region", self.region, ["us", "eu", "kr", "tw", "cn"]
        )
        self.platform_combo = self._add_option(
            options, 6, "Platform", self.platform, ["Windows", "OSX", "Android", "iOS"]
        )
        self.arch_combo = self._add_option(
            options, 8, "Architecture", self.arch, ["x86_64", "x86_32", "arm64"]
        )
        self.locale_combo = self._add_option(
            options,
            10,
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

        connections = ttk.Frame(frame)
        connections.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(0, 4))
        ttk.Label(connections, text="Download connections").pack(
            side="left", padx=(0, 8)
        )
        ttk.Label(connections, text="Global").pack(side="left")
        self.global_spin = ttk.Spinbox(
            connections,
            from_=1,
            to=512,
            width=6,
            textvariable=self.max_concurrency,
        )
        self.global_spin.pack(side="left", padx=(4, 10))
        ttk.Label(connections, text="Per host").pack(side="left")
        self.per_host_spin = ttk.Spinbox(
            connections,
            from_=1,
            to=512,
            width=6,
            textvariable=self.max_per_host,
        )
        self.per_host_spin.pack(side="left", padx=(4, 0))

        buttons = ttk.Frame(frame)
        buttons.grid(row=5, column=0, columnspan=4, sticky="ew", pady=8)
        self.download_button = ttk.Button(
            buttons, text="Download", command=self._start_download
        )
        self.download_button.pack(side="left")
        self.stop_button = ttk.Button(
            buttons, text="Stop", command=self._stop_download, state="disabled"
        )
        self.stop_button.pack(side="left", padx=(8, 0))
        self.refresh_button = ttk.Button(
            buttons, text="Refresh versions", command=self._reload_versions
        )
        self.refresh_button.pack(side="left", padx=(8, 0))
        ttk.Label(buttons, textvariable=self.status).pack(side="right")

        self.log = scrolledtext.ScrolledText(
            frame,
            wrap="word",
            height=20,
            state="disabled",
            font=("monospace", 9),
        )
        self.log.grid(row=9, column=0, columnspan=4, sticky="nsew", pady=(4, 0))

        ttk.Label(
            frame,
            text="Downloads official Blizzard CDN files. Ownership of the game is required.",
            foreground="#666666",
        ).grid(row=10, column=0, columnspan=4, sticky="w", pady=(6, 0))

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
        width: int = 14,
    ) -> ttk.Combobox:
        ttk.Label(parent, text=label).grid(
            row=0, column=column, sticky="w", padx=(0, 6)
        )
        combo = ttk.Combobox(
            parent,
            textvariable=variable,
            values=values,
            state="readonly",
            width=width,
        )
        combo.grid(row=1, column=column, sticky="ew", padx=(0, 10))
        return combo

    def _on_version_changed(self, *_args: object) -> None:
        record = self.version_records.get(self.version.get())
        if record is None:
            return
        self.build_config.set(record.get("build_config", ""))
        self.cdn_config.set(record.get("cdn_config", ""))
        version = record.get("version", "")
        current_output = Path(self.output.get()).name
        if (
            current_output.startswith("Diablo III ")
            or current_output == self.product.get()
        ):
            self.output.set(
                str(Path.home() / "Games" / f"{self.product.get()} {version}".strip())
            )

    def _on_product_or_region_changed(self, *_args: object) -> None:
        self._reload_versions()

    def _reload_versions(self) -> None:
        if self.process is not None:
            return
        self.version_load_token += 1
        token = self.version_load_token
        product = self.product.get()
        region = self.region.get()
        self.status.set(f"Loading all {product} versions…")
        self.version_combo.configure(state="disabled")
        self.refresh_button.configure(state="disabled")
        self.product_combo.configure(state="disabled")
        self.region_combo.configure(state="disabled")
        self.download_button.configure(state="disabled")
        self.version_records = {}
        self.version_combo["values"] = ["Custom"]
        self.version.set("Custom")
        if product != "d3":
            self.build_config.set("")
            self.cdn_config.set("")

        def worker() -> None:
            try:
                records = fetch_version_history(product, region)
                error = ""
            except Exception as exc:  # noqa: BLE001 - shown in status
                records = []
                error = str(exc)
            self.version_queue.put((token, product, region, records, error))

        threading.Thread(target=worker, daemon=True).start()
        self.after(100, self._drain_version_queue)

    def _drain_version_queue(self) -> None:
        try:
            token, product, region, records, error = self.version_queue.get_nowait()
        except queue.Empty:
            self.after(100, self._drain_version_queue)
            return
        if token != self.version_load_token:
            self.after(100, self._drain_version_queue)
            return

        if records:
            labels = [version_label(record) for record in records]
            self.version_records = dict(zip(labels, records, strict=True))
            self.version_combo["values"] = labels + ["Custom"]
            self.version.set(labels[0])
            self.status.set(f"Loaded {len(records)} {product} versions ({region})")
            if error:
                self._append_log(f"Version lookup warning: {error}\n")
        elif product == "d3":
            labels = list(VERSION_PRESETS)
            self.version_records = {
                label: {
                    "version": label.removeprefix("Diablo III "),
                    "build_config": preset[0],
                    "cdn_config": preset[1],
                }
                for label, preset in VERSION_PRESETS.items()
                if preset is not None
            }
            self.version_combo["values"] = labels
            self.version.set("Diablo III 2.7.4.84161")
            self.status.set("Using built-in Diablo III presets")
            self._append_log(f"Version lookup failed: {error}\n")
        else:
            self.version_combo["values"] = ["Custom"]
            self.version.set("Custom")
            self.status.set("Version lookup failed; use Custom")
            self._append_log(f"Version lookup failed: {error}\n")

        self.version_combo.configure(state="readonly")
        self.refresh_button.configure(state="normal")
        self.product_combo.configure(state="readonly")
        self.region_combo.configure(state="readonly")
        self.download_button.configure(state="normal")
        self.after(100, self._drain_version_queue)

    def _append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _browse(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.output.get())
        if selected:
            self.output.set(selected)

    def _set_running(self, running: bool) -> None:
        for widget in (
            self.browse_button,
            self.download_button,
            self.refresh_button,
        ):
            widget.configure(state="disabled" if running else "normal")
        for combo in (
            self.version_combo,
            self.product_combo,
            self.region_combo,
            self.platform_combo,
            self.arch_combo,
            self.locale_combo,
        ):
            combo.configure(state="disabled" if running else "readonly")
        for spinbox in (self.global_spin, self.per_host_spin):
            spinbox.configure(state="disabled" if running else "normal")
        self.stop_button.configure(state="normal" if running else "disabled")

    def _start_download(self) -> None:
        error = validate_inputs(
            self.build_config.get(), self.cdn_config.get(), self.output.get()
        )
        if error:
            messagebox.showerror("Invalid configuration", error)
            return
        concurrency_error = validate_concurrency(
            self.max_concurrency.get(), self.max_per_host.get()
        )
        if concurrency_error:
            messagebox.showerror("Invalid connection limits", concurrency_error)
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
        env["CASCETTE_MAX_CONCURRENCY"] = self.max_concurrency.get()
        env["CASCETTE_MAX_PER_HOST"] = self.max_per_host.get()

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


def run_cli() -> None:
    """Run the bundled Cascette CLI from a frozen Windows executable."""
    from cascette_tools.__main__ import main as cascette_main

    sys.argv[0] = "blizzget"
    cascette_main()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] != "--gui":
        run_cli()
    else:
        main()
