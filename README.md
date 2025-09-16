# Realtime PC STATS
Using a Lilygo T-Display S3 to update the status of the PC by serial connection.

Board used: https://www.amazon.com/LILYGO%C2%AE-T-Display-RP2040-Raspberry-Development/dp/B09J112YR7/ref=sr_1_2?crid=2NGUXNZSQD8SK&dib=eyJ2IjoiMSJ9.quIGGkkNawNFSGY_DSb4K4gtA3TYRX9SVfINp-cyrX1BJ7QShxIIVITBQZi4qUZ2yvy2DDIXyhg_Z5R8-sy1gPk2Z8OE-0Ry9Ebof6ECeMiryRKjp3fhFE5vT5kqaurwdYE4NQQYSyBLLbxqH_n1zG2coUeEvZju6leM8sUTNG8EWwwKeiEsu8RtGZb4eil0gDyVE6JUPBne9sfjxZjoOyW_ZiVgijKxqv3otiHze2E.2rBM2NAygNwSPo6jXkO-yB_U2dUGabulUdjTiv6ERpY&dib_tag=se&keywords=lilygo+t-display+s3&qid=1757984297&sprefix=lilygo+t-dis%2Caps%2C107&sr=8-2

Once the code is burned on the top device, you will need to run the FEEDER.PY script to get data to flow and show CPU /MEMORY / GPU usage.
You will need to know the COM ports, as this will have to be updated in the FEEDER.PY.
Could you be sure to install the following for the FEEDER.PY script to run?
python -m pip install psutil pyserial GPUtil

I have also included a FEEDER.EXE, which makes this easier.

I am no coder, but I am learning. Most of the code was reviewed by ChatGPT to get the display working, which was a pain.

