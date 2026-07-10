import requests
from concurrent.futures import ThreadPoolExecutor, as_completed

def check_host(host):
    """Check if a single host is online"""
    try:
        resp = requests.get(f"http://{host}", timeout=1.5)
        return host, resp.status_code == 200
    except:
        return host, False

def main():
    # Generate hostnames mybot1.local through mybot9.local
    hosts = [f"mybot{i}.local" for i in range(1, 10)]
    
    results = []
    
    # Check all hosts in parallel using thread pool
    with ThreadPoolExecutor(max_workers=10) as executor:
        future_to_host = {executor.submit(check_host, host): host for host in hosts}
        
        for future in as_completed(future_to_host):
            host, is_online = future.result()
            results.append((host, is_online))
    
    # Sort results by bot number for cleaner output
    results.sort(key=lambda x: int(x[0].replace('mybot', '').replace('.local', '')))
    
    # Display results
    print("\nResults:")
    print("-" * 30)
    for host, is_online in results:
        status = "ONLINE" if is_online else "OFFLINE"
        print(f"{host:20} {status}")
    
    # Show summary
    online = [h for h, status in results if status]
    print(f"\nOnline bots: {len(online)}/9")
    if online:
        print("Online: " + ", ".join(online))

if __name__ == "__main__":
    try:
        import requests
    except ImportError:
        print("Installing requests...")
        import subprocess
        import sys
        subprocess.check_call([sys.executable, "-m", "pip", "install", "requests"])
        print("Done! Please run the script again.")
        sys.exit(0)
    
    main()

