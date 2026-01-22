import asyncio
import websockets
import serial
import serial_asyncio


async def write_to_serial(queue):
    usb_port = "/dev/serial0"
    baud_rate = 57600
    reader, writer = await serial_asyncio.open_serial_connection(url=usb_port, baudrate=baud_rate)
    print("Started serial")
    try:
        while True:
            message = await queue.get()

            writer.write(message)
            await writer.drain()
    
    except Exception as e:
        print(f"Error in serial writer: {e}")
    finally:
        writer.close()
        await writer.wait_closed()


async def connect_to_websocket(queue):
    uri = "ws://103.174.103.99:8080"  # Replace with your WebSocket server URL
    try:
        async with websockets.connect(uri, ping_interval=20) as websocket:
            await websocket.send("Ubuntu5")
            await asyncio.sleep(1)
            while True:
                message = await websocket.recv()
                #print(f"{message.hex()}")
                #print(f"{message}")
                
                await queue.put(message)
    
    except Exception as e:
        print(f"Error in Websocket connection: {e}")



async def main():
    message_queue = asyncio.Queue()

    await asyncio.gather(connect_to_websocket(message_queue), write_to_serial(message_queue))


if __name__ == "__main__":
    asyncio.run(main())
    
