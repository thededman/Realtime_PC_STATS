import sys, os
import time
import threading
import queue
import traceback
import psutil
import serial
import serial.tools.list_ports
import subprocess, shutil   # for nvidia-smi fallback
import tkinter as tk
from tkinter import ttk, messagebox

# System tray support
try:
    import pystray
    from PIL import Image
    HAVE_TRAY = True
except ImportError:
    HAVE_TRAY = False

# Optional GPU (NVIDIA) via NVML
try:
    import pynvml
    HAVE_NVML = True
except Exception:
    HAVE_NVML = False

APP_TITLE = "ESP32 PC Stats Feeder"
BAUD_DEFAULT = 115200
SEND_HZ = 5                 # how many times per second to send
SEND_INTERVAL = 1.0 / SEND_HZ

# Default drives to report free space
DEFAULT_DRIVES = ["C", "D"]

def _resource_path(name: str) -> str:
    """Return absolute path to a bundled resource (PyInstaller) or local file."""
    base = getattr(sys, "_MEIPASS", None)
    if base is None:
        base = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base, name)

# ---- Windows helpers to suppress console windows for subprocess ----
if os.name == "nt":
    try:
        WIN_CREATE_NO_WINDOW = subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    except Exception:
        WIN_CREATE_NO_WINDOW = 0x08000000  # fallback
    _HIDDEN_SI = subprocess.STARTUPINFO()
    _HIDDEN_SI.dwFlags |= subprocess.STARTF_USESHOWWINDOW  # type: ignore[attr-defined]
else:
    WIN_CREATE_NO_WINDOW = 0
    _HIDDEN_SI = None

def _run_nvidia_smi_hidden(args, timeout=0.6) -> str:
    """Run nvidia-smi with no flashing console and return stdout (str)."""
    return subprocess.check_output(
        args,
        stderr=subprocess.DEVNULL,
        timeout=timeout,
        shell=False,
        startupinfo=_HIDDEN_SI,
        creationflags=WIN_CREATE_NO_WINDOW,
    ).decode("ascii", errors="ignore").strip()

def _b2s(x):
    return x.decode() if isinstance(x, (bytes, bytearray)) else str(x)

def _list_gpu_names_with_index() -> list[str]:
    """
    Return a list like ["0: NVIDIA GeForce RTX 3080", "1: NVIDIA RTX A2000"]
    Try NVML first; if unavailable, fall back to nvidia-smi; else default to ["0: GPU0"].
    """
    # NVML path
    if HAVE_NVML:
        try:
            pynvml.nvmlInit()
            count = pynvml.nvmlDeviceGetCount()
            names = []
            for i in range(count):
                h = pynvml.nvmlDeviceGetHandleByIndex(i)
                nm = pynvml.nvmlDeviceGetName(h)
                names.append(f"{i}: {_b2s(nm)}")
            pynvml.nvmlShutdown()
            if names:
                return names
        except Exception:
            # fall through to nvidia-smi
            try:
                pynvml.nvmlShutdown()
            except Exception:
                pass

    # nvidia-smi path
    smi = shutil.which("nvidia-smi")
    if smi:
        try:
            out = _run_nvidia_smi_hidden(
                [smi, "--query-gpu=name", "--format=csv,noheader"]
            )
            lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
            if lines:
                return [f"{i}: {lines[i]}" for i in range(len(lines))]
        except Exception:
            pass

    # default single entry
    return ["0: GPU0"]

class FeederThread(threading.Thread):
    def __init__(self, port_getter, baud_getter, gpu_enabled_getter,
                 disk_scale_getter, drives_getter, log_func, on_disconnect,
                 gpu_index_getter):
        super().__init__(daemon=True)
        self._stop = threading.Event()
        self.serial = None
        self.port_getter = port_getter
        self.baud_getter = baud_getter
        self.gpu_enabled_getter = gpu_enabled_getter
        self.disk_scale_getter = disk_scale_getter
        self.drives_getter = drives_getter
        self.log = log_func
        self.on_disconnect = on_disconnect
        self.gpu_index_getter = gpu_index_getter

        self.last_disk = psutil.disk_io_counters()
        self.last_time = time.time()

        # GPU backends
        self.nvml_ok = False
        self.nvml_handle = None
        self.smi_ok = False
        self.smi_path = shutil.which("nvidia-smi")

    def run(self):
        port = self.port_getter()
        baud = self.baud_getter()
        try:
            self.serial = serial.Serial(port, baud, timeout=0)
            self.log(f"Connected: {port} @ {baud}")
        except Exception as e:
            self.log(f"ERROR: Could not open {port}: {e}")
            self.on_disconnect()
            return

        # --- GPU init: NVML → fallback to nvidia-smi ---
        if self.gpu_enabled_getter():
            idx = int(self.gpu_index_getter())
            if HAVE_NVML:
                try:
                    # If your system needs an explicit DLL dir, uncomment:
                    # os.add_dll_directory(r"C:\Program Files\NVIDIA Corporation\NVSMI")
                    pynvml.nvmlInit()
                    self.nvml_handle = pynvml.nvmlDeviceGetHandleByIndex(idx)
                    self.nvml_ok = True
                    self.log(f"NVML initialized (GPU {idx}).")
                except Exception as e:
                    self.log(f"NVML init failed: {e}")
                    self.nvml_ok = False
            if not self.nvml_ok:
                if self.smi_path:
                    self.smi_ok = True
                    self.log(f"Using nvidia-smi fallback (GPU {idx}): {self.smi_path}")
                else:
                    self.log("GPU fallback disabled: nvidia-smi not found in PATH.")

        # Warm up CPU % counter (non-blocking)
        psutil.cpu_percent(interval=None)

        try:
            while not self._stop.is_set():
                t0 = time.time()

                cpu = psutil.cpu_percent(interval=None)
                mem = psutil.virtual_memory().percent

                # Disk throughput → MB/s + percent scaled by user "100% = X MB/s"
                now = time.time()
                dio = psutil.disk_io_counters()
                dt = max(1e-6, now - self.last_time)
                read_mb_s = (dio.read_bytes - self.last_disk.read_bytes) / (1024*1024) / dt
                write_mb_s = (dio.write_bytes - self.last_disk.write_bytes) / (1024*1024) / dt
                mbps = read_mb_s + write_mb_s
                self.last_disk = dio
                self.last_time = now

                disk_scale = max(1.0, float(self.disk_scale_getter()))  # avoid divide by zero
                disk_pct = max(0.0, min(100.0, (mbps / disk_scale) * 100.0))

                # Temps / Free space:
                cpu_temp_f = -999.0
                try:
                    if hasattr(psutil, "sensors_temperatures"):
                        temps = psutil.sensors_temperatures(fahrenheit=False) or {}
                        for key in ("coretemp", "k10temp", "acpitz", "cpu-thermal", "nvme"):
                            if key in temps and temps[key]:
                                c = temps[key][0].current
                                cpu_temp_f = c * 9.0/5.0 + 32.0
                                break
                except Exception:
                    pass

                # --- GPU read (NVML → nvidia-smi → zeros) ---
                gpu = 0.0
                gpu_temp_f = -999.0
                if self.gpu_enabled_getter():
                    idx = int(self.gpu_index_getter())
                    if self.nvml_ok:
                        try:
                            util = pynvml.nvmlDeviceGetUtilizationRates(self.nvml_handle)
                            gpu = float(util.gpu)
                            tC = pynvml.nvmlDeviceGetTemperature(
                                self.nvml_handle, pynvml.NVML_TEMPERATURE_GPU
                            )
                            gpu_temp_f = tC * 9.0/5.0 + 32.0
                        except Exception:
                            pass
                    elif self.smi_ok:
                        try:
                            out = _run_nvidia_smi_hidden(
                                [self.smi_path,
                                 f"--id={idx}",
                                 "--query-gpu=utilization.gpu,temperature.gpu",
                                 "--format=csv,noheader,nounits"],
                                timeout=0.6
                            )
                            # Example: "12, 45"
                            parts = out.split(",")
                            if len(parts) >= 2:
                                gpu = float(parts[0].strip())
                                tC = float(parts[1].strip())
                                gpu_temp_f = tC * 9.0/5.0 + 32.0
                        except Exception:
                            pass

                # Free space for selected drives
                drives = self.drives_getter()
                free_gb = []
                for d in drives:
                    try:
                        usage = psutil.disk_usage(f"{d}:/")
                        free_gb.append(usage.free / (1024**3))
                    except Exception:
                        free_gb.append(-1.0)

                # Ensure at least two fields for C and D positions
                freeC = free_gb[0] if len(free_gb) > 0 else -1.0
                freeD = free_gb[1] if len(free_gb) > 1 else -1.0

                # CSV: cpu,mem,gpu,diskPct,diskMBps,cpuTempF,gpuTempF,freeC_GB,freeD_GB
                line = f"{cpu:.1f},{mem:.1f},{gpu:.1f},{disk_pct:.1f},{mbps:.2f},{cpu_temp_f:.1f},{gpu_temp_f:.1f},{freeC:.0f},{freeD:.0f}\n"
                try:
                    self.serial.write(line.encode("ascii"))
                except Exception as e:
                    self.log(f"Serial write failed: {e}")
                    break

                # Update status line in GUI
                self.log(
                    f"CPU {cpu:.0f}%  MEM {mem:.0f}%  GPU {gpu:.0f}% (GPU{int(self.gpu_index_getter())})  "
                    f"DISK {disk_pct:.0f}% ({mbps:.1f} MB/s)  "
                    f"CPUt {cpu_temp_f:.0f}F  GPUt {gpu_temp_f:.0f}F  "
                    f"C: {freeC:.0f} GB  D: {freeD:.0f} GB",
                    replace_line=True
                )

                # pacing
                elapsed = time.time() - t0
                wait = max(0.0, SEND_INTERVAL - elapsed)
                time.sleep(wait)

        except Exception as e:
            self.log("Feeder crashed:\n" + "".join(traceback.format_exception_only(type(e), e)))
        finally:
            try:
                if self.serial and self.serial.is_open:
                    self.serial.close()
            except Exception:
                pass

            if self.nvml_ok:
                try:
                    pynvml.nvmlShutdown()
                except Exception:
                    pass

            self.on_disconnect()
            self.log("Disconnected.")

    def stop(self):
        self._stop.set()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.resizable(False, False)

        # Set window icon (also works inside PyInstaller bundle)
        try:
            self.iconbitmap(_resource_path("app.ico"))
        except Exception:
            pass  # icon is optional

        # state
        self.feeder = None
        self.ui_queue = queue.Queue()
        self.last_log_replace = False

        # System tray
        self.tray_icon = None
        self._setup_tray()

        # --- UI ---
        frm = ttk.Frame(self, padding=10)
        frm.grid(column=0, row=0, sticky="nsew")

        # Row 0: Port + Refresh + Baud
        ttk.Label(frm, text="Serial Port:").grid(column=0, row=0, sticky="w")
        self.port_cmb = ttk.Combobox(frm, width=22, state="readonly", values=self._list_ports())
        self.port_cmb.grid(column=1, row=0, sticky="w")
        if self.port_cmb["values"]:
            self.port_cmb.current(0)

        self.refresh_btn = ttk.Button(frm, text="Refresh", command=self._refresh_ports)
        self.refresh_btn.grid(column=2, row=0, padx=(6, 0))

        ttk.Label(frm, text="Baud:").grid(column=3, row=0, padx=(12, 0), sticky="e")
        self.baud_cmb = ttk.Combobox(frm, width=8, state="readonly",
                                     values=("115200", "230400", "460800", "921600"))
        self.baud_cmb.grid(column=4, row=0, sticky="w")
        self.baud_cmb.set(str(BAUD_DEFAULT))

        # Row 1: GPU enable + disk scale + drives
        self.gpu_var = tk.BooleanVar(value=True)
        self.gpu_chk = ttk.Checkbutton(frm, text="Enable GPU (NVML or nvidia-smi fallback)", variable=self.gpu_var)
        self.gpu_chk.grid(column=0, row=1, columnspan=2, sticky="w", pady=(8, 0))

        ttk.Label(frm, text="Disk 100% =").grid(column=2, row=1, sticky="e", padx=(10, 2), pady=(8, 0))
        self.disk_scale = tk.DoubleVar(value=200.0)  # MB/s that maps to 100%
        self.disk_entry = ttk.Entry(frm, textvariable=self.disk_scale, width=7)
        self.disk_entry.grid(column=3, row=1, sticky="w", pady=(8, 0))
        ttk.Label(frm, text="MB/s").grid(column=4, row=1, sticky="w", pady=(8, 0))

        ttk.Label(frm, text="Drives (comma):").grid(column=0, row=2, sticky="w", pady=(8, 0))
        self.drives_var = tk.StringVar(value=",".join(DEFAULT_DRIVES))
        self.drives_entry = ttk.Entry(frm, textvariable=self.drives_var, width=18)
        self.drives_entry.grid(column=1, row=2, sticky="w", pady=(8, 0))

        # Row 2 (right side): GPU picker with names
        ttk.Label(frm, text="GPU:").grid(column=2, row=2, sticky="e", padx=(10, 4))
        gpu_list = _list_gpu_names_with_index()
        self.gpu_index_cmb = ttk.Combobox(frm, width=40, state="readonly", values=gpu_list)
        self.gpu_index_cmb.grid(column=3, row=2, columnspan=2, sticky="w")
        self.gpu_index_cmb.set(gpu_list[0])

        # Row 3: Connect / Disconnect / Minimize to Tray
        self.connect_btn = ttk.Button(frm, text="Connect", command=self._connect)
        self.connect_btn.grid(column=0, row=3, pady=(10, 0), sticky="w")
        self.disconnect_btn = ttk.Button(frm, text="Disconnect", command=self._disconnect, state="disabled")
        self.disconnect_btn.grid(column=1, row=3, pady=(10, 0), sticky="w")

        # Minimize to tray button (only if pystray is available)
        if HAVE_TRAY:
            self.tray_btn = ttk.Button(frm, text="Minimize to Tray", command=self._minimize_to_tray)
            self.tray_btn.grid(column=2, row=3, pady=(10, 0), padx=(6, 0), sticky="w")

        # Row 4: Status text
        self.status = tk.Text(frm, height=8, width=70, wrap="word")
        self.status.grid(column=0, row=4, columnspan=5, pady=(12, 0))
        self.status.configure(state="disabled")

        # Timer to process UI queue
        self.after(50, self._process_log_queue)

        # Close handler
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- UI helpers ----
    def _list_ports(self):
        ports = serial.tools.list_ports.comports()
        return [p.device for p in ports]

    def _refresh_ports(self):
        vals = self._list_ports()
        self.port_cmb["values"] = vals
        if vals:
            self.port_cmb.current(0)

    def _get_port(self):
        return self.port_cmb.get().strip()

    def _get_baud(self):
        try:
            return int(self.baud_cmb.get())
        except Exception:
            return BAUD_DEFAULT

    def _get_gpu_enabled(self):
        return bool(self.gpu_var.get())

    def _get_disk_scale(self):
        try:
            return float(self.disk_scale.get())
        except Exception:
            return 200.0

    def _get_drives(self):
        s = self.drives_var.get().strip()
        if not s:
            return DEFAULT_DRIVES
        parts = [x.strip().upper().rstrip(":") for x in s.split(",") if x.strip()]
        return parts or DEFAULT_DRIVES

    def _get_gpu_index(self):
        try:
            # "0: NVIDIA GeForce ..." -> 0
            return int(self.gpu_index_cmb.get().split(":")[0])
        except Exception:
            return 0

    def log(self, msg, replace_line=False):
        self.ui_queue.put((msg, replace_line))

    def _process_log_queue(self):
        try:
            while True:
                msg, replace = self.ui_queue.get_nowait()
                self.status.configure(state="normal")
                if replace and self.last_log_replace:
                    # delete last line
                    self.status.delete("end-2l", "end-1l")
                self.status.insert("end", (msg + "\n"))
                self.status.see("end")
                self.status.configure(state="disabled")
                self.last_log_replace = replace
        except queue.Empty:
            pass
        self.after(50, self._process_log_queue)

    # ---- Connect / Disconnect ----
    def _connect(self):
        if self.feeder is not None:
            return
        port = self._get_port()
        if not port:
            messagebox.showwarning(APP_TITLE, "No serial port selected.")
            return

        self.connect_btn.config(state="disabled")
        self.disconnect_btn.config(state="normal")
        self.feeder = FeederThread(
            port_getter=self._get_port,
            baud_getter=self._get_baud,
            gpu_enabled_getter=self._get_gpu_enabled,
            disk_scale_getter=self._get_disk_scale,
            drives_getter=self._get_drives,
            log_func=self.log,
            on_disconnect=self._on_thread_disconnected,
            gpu_index_getter=self._get_gpu_index
        )
        self.feeder.start()

    def _disconnect(self):
        if self.feeder:
            self.feeder.stop()
            self.feeder = None
        self.connect_btn.config(state="normal")
        self.disconnect_btn.config(state="disabled")

    def _on_thread_disconnected(self):
        def _ui():
            self.feeder = None
            self.connect_btn.config(state="normal")
            self.disconnect_btn.config(state="disabled")
        self.after(0, _ui)

    # ---- System Tray ----
    def _setup_tray(self):
        """Initialize the system tray icon (hidden until minimize)."""
        if not HAVE_TRAY:
            return

        # Try to load the app icon, otherwise create a simple default icon
        icon_image = None
        try:
            ico_path = _resource_path("app.ico")
            if os.path.exists(ico_path):
                icon_image = Image.open(ico_path)
        except Exception:
            pass

        # Fallback: create a simple colored square icon
        if icon_image is None:
            icon_image = Image.new("RGB", (64, 64), color=(0, 120, 212))

        # Create tray menu
        menu = pystray.Menu(
            pystray.MenuItem("Show", self._restore_from_tray, default=True),
            pystray.MenuItem("Exit", self._exit_from_tray)
        )

        self.tray_icon = pystray.Icon(APP_TITLE, icon_image, APP_TITLE, menu)

    def _minimize_to_tray(self):
        """Hide window and show system tray icon."""
        if not HAVE_TRAY or self.tray_icon is None:
            return

        self.withdraw()  # Hide the window

        # Run tray icon in a separate thread
        def run_tray():
            self.tray_icon.run()

        threading.Thread(target=run_tray, daemon=True).start()

    def _restore_from_tray(self, icon=None, item=None):
        """Restore window from system tray."""
        if self.tray_icon is not None:
            self.tray_icon.stop()

        # Schedule UI update on main thread
        self.after(0, self._show_window)

    def _show_window(self):
        """Show and focus the window."""
        self.deiconify()  # Show the window
        self.lift()       # Bring to front
        self.focus_force()
        # Re-setup tray icon for next minimize
        self._setup_tray()

    def _exit_from_tray(self, icon=None, item=None):
        """Exit application from tray menu."""
        if self.tray_icon is not None:
            self.tray_icon.stop()
        self.after(0, self._on_close)

    def _on_close(self):
        try:
            if self.feeder:
                self.feeder.stop()
                time.sleep(0.15)
        except Exception:
            pass
        try:
            if self.tray_icon is not None:
                self.tray_icon.stop()
        except Exception:
            pass
        self.destroy()


def main():
    app = App()
    app.mainloop()

if __name__ == "__main__":
    main()
