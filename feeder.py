import time
import psutil
import serial

try:
    import GPUtil
except Exception:
    GPUtil = None

# ------- CONFIG -------
PORT = "COM4"        # <-- change to your board's COM port
BAUD = 115200
INTERVAL = 1.0       # seconds between samples
MAX_IO_MBPS = 500.0  # scale for 100% (tune for your SSD/NVMe; e.g., 300, 500, 1000)

# Open serial
ser = serial.Serial(PORT, BAUD, timeout=1)

# Prime disk counters
prev = psutil.disk_io_counters(nowrap=True)
prev_time = time.time()

while True:
    now = time.time()

    # CPU / MEM
    cpu = psutil.cpu_percent(interval=None)  # non-blocking; based on last call
    mem = psutil.virtual_memory().percent

    # GPU (optional)
    gpu = 0.0
    if GPUtil:
        try:
            g = GPUtil.getGPUs()
            if g:
                gpu = float(g[0].load) * 100.0
        except Exception:
            gpu = 0.0

    # Disk I/O (MB/s)
    cur = psutil.disk_io_counters(nowrap=True)
    dt = now - prev_time
    mbps = 0.0
    if dt > 0:
        bytes_delta = (cur.read_bytes - prev.read_bytes) + (cur.write_bytes - prev.write_bytes)
        mbps = (bytes_delta / dt) / (1024 * 1024)

    # Map to 0..100%
    disk_pct = 0.0
    if MAX_IO_MBPS > 0:
        val = (mbps / MAX_IO_MBPS) * 100.0
        disk_pct = max(0.0, min(100.0, val))

    # Send line: cpu,mem,gpu,diskPct,diskMBps
    line = f"{cpu:.1f},{mem:.1f},{gpu:.1f},{disk_pct:.1f},{mbps:.1f}\n"
    ser.write(line.encode("utf-8"))

    # Update counters
    prev, prev_time = cur, now

    time.sleep(INTERVAL)
