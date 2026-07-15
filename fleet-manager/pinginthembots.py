import sys
import subprocess
import csv
import datetime
import os
import platform
import re

def get_shipped_bots(yaml_path):
    shipped = []
    if os.path.exists(yaml_path):
        with open(yaml_path, 'r', encoding='utf-8') as f:
            in_shipped = False
            for line in f:
                stripped = line.strip()
                if stripped.startswith('shipped_bots:'):
                    in_shipped = True
                    continue
                
                # If line is not empty and has no indentation (and isn't a comment or list item), we left the block
                if in_shipped and stripped and not line.startswith(' ') and not stripped.startswith('-') and not stripped.startswith('#'):
                    in_shipped = False
                    
                if in_shipped:
                    # Handle dictionary format (e.g. "- hostname: paulbot9.local" or "hostname: paulbot9.local")
                    if 'hostname:' in stripped:
                        bot = stripped.split('hostname:')[1].strip()
                        shipped.append(bot)
                    # Handle old simple list format
                    elif stripped.startswith('- ') and ':' not in stripped:
                        bot = stripped[2:].strip()
                        shipped.append(bot)
    return shipped

def ping_bot(hostname):
    """Pings a hostname and returns (is_alive, ip_address)."""
    # Use cross-platform ping arguments
    param = '-n' if platform.system().lower() == 'windows' else '-c'
    timeout_param = '-w' if platform.system().lower() == 'windows' else '-W'
    timeout_val = '1000' if platform.system().lower() == 'windows' else '1'
    
    command = ['ping', param, '1', timeout_param, timeout_val, hostname]
    
    try:
        output = subprocess.check_output(command, stderr=subprocess.STDOUT, universal_newlines=True)
        # Try to extract IP address
        ip_match = re.search(r'\[([0-9\.]+)\]', output)
        if not ip_match:
            ip_match = re.search(r'Reply from ([0-9\.]+):', output)
        
        ip_addr = ip_match.group(1) if ip_match else "Unknown"
        
        # A response with TTL usually indicates successful ping
        if "TTL=" in output or "ttl=" in output:
             return True, ip_addr
        return False, ip_addr
    except subprocess.CalledProcessError as e:
        # Ping command failed (host unreachable, timeout, etc.)
        output = e.output
        ip_match = re.search(r'\[([0-9\.]+)\]', output)
        if not ip_match:
            ip_match = re.search(r'Reply from ([0-9\.]+):', output)
            
        ip_addr = ip_match.group(1) if ip_match else "N/A"
        return False, ip_addr

def main():
    default_bots = ["rfbot", "mybot", "carbot", "paulbot", "simplebot"]
    
    # Determine which bot base names to ping
    if len(sys.argv) > 1:
        base_names = sys.argv[1:]
    else:
        base_names = default_bots
        
    print(f"Pinging bots with base names: {', '.join(base_names)}")
    
    # Prepare CSV log file in docs folder
    log_filename = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'docs', 'ping_log.csv')
    file_exists = os.path.isfile(log_filename)
    
    with open(log_filename, mode='a', newline='', encoding='utf-8') as csv_file:
        fieldnames = ['Timestamp', 'BotName', 'Status', 'IPAddress']
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        
        # Write header only if file is newly created
        if not file_exists:
            writer.writeheader()
            
        # Get list of shipped bots from fleet.yaml
        yaml_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'docs', 'fleet.yaml')
        shipped_bots = get_shipped_bots(yaml_path)

        # Ping each bot from 0 to 9
        for base in base_names:
            for i in range(10):
                bot_name = f"{base}{i}.local"
                
                if bot_name in shipped_bots:
                    print(f"Skipping {bot_name:<14} [Shipped to customer]")
                    continue
                    
                print(f"Pinging {bot_name:<15}...", end=" ", flush=True)
                
                is_alive, ip_addr = ping_bot(bot_name)
                timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                
                status_str = "Up" if is_alive else "Down"
                print(f"[{status_str}] - IP: {ip_addr}")
                
                # Append to CSV
                writer.writerow({
                    'Timestamp': timestamp,
                    'BotName': bot_name,
                    'Status': status_str,
                    'IPAddress': ip_addr
                })

    print(f"\nDone! Results saved and appended to {os.path.abspath(log_filename)}")

if __name__ == "__main__":
    main()
