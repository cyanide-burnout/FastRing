import grpc
from concurrent import futures
import time
 
# python3 -m pip install grpcio grpcio-tools
# python3 -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. gRPCTest.proto

import gRPCTest_pb2
import gRPCTest_pb2_grpc

class EchoerServicer(gRPCTest_pb2_grpc.EchoerServicer):

    def UnaryEcho(self, request, context):
        return gRPCTest_pb2.EchoReply(text=f"echo: {request.text}")

    def StreamingEcho(self, iterator, context):
        for number, request in enumerate(iterator, start=1):
            time.sleep(0.1)
            yield gRPCTest_pb2.EchoReply(text=f"{number}: {request.text}")

def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10), compression=grpc.Compression.Gzip)
    gRPCTest_pb2_grpc.add_EchoerServicer_to_server(EchoerServicer(), server)

    server.add_insecure_port("127.0.0.1:50051")
    server.start()
    server.wait_for_termination()

if __name__ == "__main__":
    serve()
