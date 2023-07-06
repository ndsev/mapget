import argparse
import mapget


def serve(args):
    print(f'Starting server on port {args.port} with datasources {args.datasource}')
    srv = mapget.Service()
    if args.datasource:
        for ds in args.datasource:
            host, port = ds.split(":")
            srv.add_remote_datasource(host, int(port))
    if args.webapp:
        if not srv.mount(args.webapp):
            print(f"Failed to mount web app {args.webapp}.")
            exit(1)
    srv.go(port=args.port)
    srv.wait_for_signal()


def fetch(args):
    print(f'Connecting client to server {args.server} for map {args.map} and layer {args.layer} with tiles {args.tile}')
    host, port = args.server.split(":")
    if args.tile:
        cli = mapget.Client(host, int(port))
        for result in cli.request(mapget.Request(args.map, args.layer, list(map(int, args.tile)))):
            print(result.geojson())


def main():
    parser = argparse.ArgumentParser(prog="mapget", description="A client/server application for map data retrieval.")

    subparsers = parser.add_subparsers()

    server_parser = subparsers.add_parser('serve', help="Starts the server.")
    server_parser.add_argument('-p', '--port', type=int, default=0, help="Port to start the server on. Default is 0.")
    server_parser.add_argument('-d', '--datasource', action='append', help="Datasources for the server in format <host:port>. Can be specified multiple times.")
    server_parser.add_argument('-w', '--webapp', help="Serve a static web application, in the format `[<url-scope>:]<filesystem-path>`.")
    server_parser.set_defaults(func=serve)

    client_parser = subparsers.add_parser('fetch', help="Connects to the server to fetch tiles.")
    client_parser.add_argument('-s', '--server', required=True, help="Server to connect to in format <host:port>.")
    client_parser.add_argument('-m', '--map', required=True, help="Map to retrieve.")
    client_parser.add_argument('-l', '--layer', required=True, help="Layer of the map to retrieve.")
    client_parser.add_argument('-t', '--tile', type=int, required=True, action='append', help="Tile of the map to retrieve. Can be specified multiple times.")
    client_parser.set_defaults(func=fetch)

    args = parser.parse_args()
    if hasattr(args, 'func'):
        args.func(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
