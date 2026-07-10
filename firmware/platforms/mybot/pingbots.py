import requests
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

def check_host(host):
    """Check if a single host is online"""
    try:
        resp = requests.get(f"http://{host}", timeout=1.5)
        return host, resp.status_code == 200
    except:
        return host, False

def main():
    # Get bot prefix from command line argument
    if len(sys.argv) < 2:
        print("Usage: python mybotpinger.py <bot_prefix>")
        print("Example: python mybotpinger.py mybot")
        print("Example: python mybotpinger.py paulbot")
        sys.exit(1)
    
    bot_prefix = sys.argv[1]
    
    # Generate hostnames like <prefix>1.local through <prefix>9.local
    hosts = [f"{bot_prefix}{i}.local" for i in range(1, 10)]

    results = []

    # Check all hosts in parallel using thread pool
    with ThreadPoolExecutor(max_workers=10) as executor:
        future_to_host = {executor.submit(check_host, host): host for host in hosts}

        for future in as_completed(future_to_host):
            host, is_online = future.result()
            results.append((host, is_online))
            status = "ONLINE" if is_online else "OFFLINE"
            print(f"{host}: {status}")

    # Summary
    online_count = sum(1 for _, is_online in results if is_online)
    print(f"\nSummary: {online_count}/{len(results)} hosts online")

if __name__ == "__main__":
    main()


