import sys
import time
import threading
import queue
import traceback
import psutil
import serial
import serial.tools.list_ports

# Optional GPU (NVIDIA) via NVML
try:
    import pynvml
    HAVE_NVML = True
except Exception:
    HAVE_NVML = False

import tkinter as tk
from tkinter import ttk, messagebox

APP_TITLE = "PC Stats Feeder"
BAUD_DEFAULT = 115200
SEND_HZ = 5                 # how many times per second to send
SEND_INTERVAL = 1.0 / SEND_HZ

# Default drives to report free space
DEFAULT_DRIVES = ["C", "D"]

class FeederThread(threading.Thread):
    def __init__(self, port_getter, baud_getter, gpu_enabled_getter,
                 disk_scale_getter, drives_getter, log_func, on_disconnect):
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

        self.last_disk = psutil.disk_io_counters()
        self.last_time = time.time()

        self.nvml_ok = False
        self.nvml_handle = None

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

        # NVML (GPU) init if requested and available
        if self.gpu_enabled_getter() and HAVE_NVML:
            try:
                pynvml.nvmlInit()
                self.nvml_handle = pynvml.nvmlDeviceGetHandleByIndex(0)
                self.nvml_ok = True
                self.log("NVML initialized (GPU 0).")
            except Exception as e:
                self.log(f"NVML init failed: {e}")
                self.nvml_ok = False

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
                    # Windows “CPU Package” temp may not always be available via psutil.
                    # If psutil has sensors_temperatures, try common keys:
                    if hasattr(psutil, "sensors_temperatures"):
                        temps = psutil.sensors_temperatures(fahrenheit=False) or {}
                        # try common keys in order
                        for key in ("coretemp", "k10temp", "acpitz", "cpu-thermal", "nvme"):
                            if key in temps and temps[key]:
                                # take first reading
                                c = temps[key][0].current
                                cpu_temp_f = c * 9.0/5.0 + 32.0
                                break
                except Exception:
                    pass

                gpu = 0.0
                gpu_temp_f = -999.0
                if self.gpu_enabled_getter() and self.nvml_ok:
                    try:
                        util = pynvml.nvmlDeviceGetUtilizationRates(self.nvml_handle)
                        gpu = float(util.gpu)  # percent

                        tC = pynvml.nvmlDeviceGetTemperature(
                            self.nvml_handle, pynvml.NVML_TEMPERATURE_GPU
                        )
                        gpu_temp_f = tC * 9.0/5.0 + 32.0
                    except Exception:
                        # if GPU disappears, keep zeros
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
                    f"CPU {cpu:.0f}%  MEM {mem:.0f}%  GPU {gpu:.0f}%  DISK {disk_pct:.0f}% ({mbps:.1f} MB/s)  "
                    f"CPUt {cpu_temp_f:.0f}F  GPUt {gpu_temp_f:.0f}F  C: {freeC:.0f} GB  D: {freeD:.0f} GB",
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

        # state
        self.feeder = None
        self.ui_queue = queue.Queue()
        self.last_log_replace = False

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
        self.gpu_var = tk.BooleanVar(value=True and HAVE_NVML)
        self.gpu_chk = ttk.Checkbutton(frm, text=f"Enable GPU (NVML){'' if HAVE_NVML else ' - not installed'}",
                                       variable=self.gpu_var)
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

        # Row 3: Connect / Disconnect
        self.connect_btn = ttk.Button(frm, text="Connect", command=self._connect)
        self.connect_btn.grid(column=0, row=3, pady=(10, 0), sticky="w")
        self.disconnect_btn = ttk.Button(frm, text="Disconnect", command=self._disconnect, state="disabled")
        self.disconnect_btn.grid(column=1, row=3, pady=(10, 0), sticky="w")

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
            on_disconnect=self._on_thread_disconnected
        )
        self.feeder.start()

    def _disconnect(self):
        if self.feeder:
            self.feeder.stop()
            self.feeder = None
        self.connect_btn.config(state="normal")
        self.disconnect_btn.config(state="disabled")

    def _on_thread_disconnected(self):
        # Called from feeder thread on its way out
        def _ui():
            self.feeder = None
            self.connect_btn.config(state="normal")
            self.disconnect_btn.config(state="disabled")
        self.after(0, _ui)

    def _on_close(self):
        try:
            if self.feeder:
                self.feeder.stop()
                # small grace period
                time.sleep(0.15)
        except Exception:
            pass
        self.destroy()


def main():
    app = App()
    app.mainloop()

if __name__ == "__main__":
    main()
