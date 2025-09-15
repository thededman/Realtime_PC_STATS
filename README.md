# Realtime_PC_STATS
Using a Lilygo T-Display s3 to update status of PC by serial connection.

Once the code is burned on the top device, you will need to run the FEEDER.PY script to get data to flow and show CPU /MEMORY / GPU usage.
You will need to know the COM ports, as this will have to be updated in the FEEDER.PY.
Be sure to install the follow for the FEEDER.PY script to run.
python -m pip install psutil pyserial GPUtil


I am no coder, but I am learning. Most of the code was reviewed by ChatGPT to get the display working, which was a pain.

