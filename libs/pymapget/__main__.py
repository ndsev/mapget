import argparse
import mapget


def server(args):
    print(f'Starting server on port {args.port} with datasources {args.datasource}')
    srv = mapget.Service()
    if args.datasource:
        for ds in args.datasource:
            host, port = ds.split(":")
            srv.add_remote_datasource(host, int(port))
    srv.go(port=args.port)
    srv.wait_for_signal()


def client(args):
    print(f'Connecting client to server {args.server} for map {args.map} and layer {args.layer} with tiles {args.tile}')
    raise NotImplementedError()


def main():
    parser = argparse.ArgumentParser(prog="mapget", description="A client/server application for map data retrieval.")

    subparsers = parser.add_subparsers()

    server_parser = subparsers.add_parser('server', help="Starts the server.")
    server_parser.add_argument('-p', '--port', type=int, default=0, help="Port to start the server on. Default is 0.")
    server_parser.add_argument('-d', '--datasource', action='append', help="Datasources for the server in format <host:port>. Can be specified multiple times.")
    server_parser.set_defaults(func=server)

    client_parser = subparsers.add_parser('client', help="Connects to the server.")
    client_parser.add_argument('-s', '--server', required=True, help="Server to connect to in format <host:port>.")
    client_parser.add_argument('-m', '--map', required=True, help="Map to retrieve.")
    client_parser.add_argument('-l', '--layer', help="Layer of the map to retrieve.")
    client_parser.add_argument('-t', '--tile', type=int, required=True, action='append', help="Tile of the map to retrieve. Can be specified multiple times.")
    client_parser.set_defaults(func=client)

    args = parser.parse_args()
    if hasattr(args, 'func'):
        args.func(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
