import asyncio
import websockets

pi1_connected_client = None

async def handle_client(websocket, path):
    # Register client
    
    global pi1_connected_client

    try:
        message = await websocket.recv()
        if message == "Ubuntu5" and pi1_connected_client is None:
            pi1_connected_client = websocket
            print(f"Ubuntu5 connected from {websocket.remote_address}")
        else:
            print(f"New registered client from {websocket.remote_address}")


        async for message in websocket:
            #Echo the message to pi1
            if pi1_connected_client and websocket != pi1_connected_client:
                await pi1_connected_client.send(message)           
                                                                                
    except websockets.ConnectionClosed:
        print(f"Client disconnected from {websocket.remote_address}")
        
    finally:
        #Unregister client
        if websocket == pi1_connected_client:
            pi1_connected_client = None

        
async def start_server():
    server = await websockets.serve(handle_client, "0.0.0.0", 8080, ping_interval=20)
    print("WebSocket server started on ws://0.0.0.0:8080")
    await server.wait_closed()


asyncio.run(start_server())

