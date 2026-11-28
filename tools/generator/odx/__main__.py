# SSAS - Simple Smart Automotive Software
# Copyright (C) 2022 Parai Wang <parai@foxmail.com>

if __name__ == "__main__":
    from odx import *
    import argparse
    import json
    import os

    parser = argparse.ArgumentParser(description="ODX tools")
    parser.add_argument("-i", "--input", help="input ODX file", type=str, required=True)
    parser.add_argument("-o", "--output", help="output ODX json", type=str, default=None, required=False)
    args = parser.parse_args()
    op = ODXParser(args.input)
    services = op.getServices()
    if args.output != None:
        with open(args.output, "w") as f:
            json.dump(services, f, indent=2)
    else:
        print(services)
