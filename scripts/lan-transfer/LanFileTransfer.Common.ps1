# Shared engine for Send-LanFile.ps1 and Receive-LanFile.ps1.
# Windows PowerShell 5.1 or PowerShell 7 on Windows. No external dependencies.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-TransferSecret {
    param([System.Security.SecureString] $Secret)
    if ($null -eq $Secret) { $Secret = Read-Host 'Shared secret (same on both machines, at least 16 characters)' -AsSecureString }
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Secret)
    try {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
        if ($plain.Length -lt 16) { throw 'Use a shared secret of at least 16 characters.' }
        return $plain
    } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

if (-not ('LanTransferV1.Engine' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Diagnostics;

namespace LanTransferV1 {
    public static class Engine {
        const int BufferSize = 1024 * 1024;
        const int Timeout = 300000;
        static readonly Encoding Utf8 = new UTF8Encoding(false, true);

        // Bounded, explicitly framed messages. File bytes are streamed separately.
        static void Put(BinaryWriter w, string value) {
            byte[] bytes = Utf8.GetBytes(value);
            w.Write(bytes.Length); w.Write(bytes); w.Flush();
        }
        static string Get(BinaryReader r) {
            int length = r.ReadInt32();
            if (length < 0 || length > 16384) throw new IOException("Invalid message length.");
            byte[] bytes = r.ReadBytes(length);
            if (bytes.Length != length) throw new EndOfStreamException("Connection closed.");
            return Utf8.GetString(bytes);
        }
        static string Hex(byte[] value) { return BitConverter.ToString(value).Replace("-", "").ToLowerInvariant(); }
        static string Hash(Stream stream) {
            using (SHA256 sha = SHA256.Create()) { return Hex(sha.ComputeHash(stream)); }
        }
        static string Proof(string secret, string role, string challenge) {
            using (HMACSHA256 mac = new HMACSHA256(Utf8.GetBytes(secret))) {
                return Hex(mac.ComputeHash(Utf8.GetBytes(role + ":" + challenge)));
            }
        }
        static bool Equal(string a, string b) {
            if (a.Length != b.Length) return false;
            int diff = 0;
            for (int i = 0; i < a.Length; i++) diff |= a[i] ^ b[i];
            return diff == 0;
        }
        static string Nonce() {
            byte[] bytes = new byte[32];
            using (RandomNumberGenerator rng = RandomNumberGenerator.Create()) { rng.GetBytes(bytes); }
            return Hex(bytes);
        }
        static bool ValidHash(string value) {
            if (value.Length != 64) return false;
            foreach (char c in value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return true;
        }
        static void CheckName(string name) {
            if (String.IsNullOrWhiteSpace(name) || name.Length > 200 || name != Path.GetFileName(name) ||
                name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.EndsWith(".") || name.EndsWith(" ") ||
                name == "." || name == ".." || name.Equals(".lan-transfer", StringComparison.OrdinalIgnoreCase))
                throw new IOException("Invalid destination filename (maximum 200 characters).");
            string stem = name.Split('.')[0].TrimEnd(' ').ToUpperInvariant();
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                stem == "CONIN$" || stem == "CONOUT$" ||
                (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) &&
                 "123456789\u00b9\u00b2\u00b3".IndexOf(stem[3]) >= 0))
                throw new IOException("Reserved Windows filename.");
        }
        static void CheckDirectory(string path) {
            Directory.CreateDirectory(path);
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Transfer directories must not be junctions or symbolic links: " + path);
        }
        static void Progress(long done, long total, Stopwatch watch, long start) {
            double seconds = Math.Max(watch.Elapsed.TotalSeconds, 0.001);
            Console.WriteLine("{0:N1}%  {1:N1} / {2:N1} MiB  ({3:N1} MiB/s)",
                total == 0 ? 100.0 : 100.0 * done / total, done / 1048576.0,
                total / 1048576.0, (done - start) / 1048576.0 / seconds);
        }

        public static void Send(string path, string host, int port, string secret, string name) {
            CheckName(name);
            // Keep the source locked against writes for hashing and the entire transfer.
            using (FileStream file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, BufferSize, FileOptions.SequentialScan)) {
                Console.WriteLine("Hashing source ({0:N1} MiB)...", file.Length / 1048576.0);
                string hash = Hash(file); file.Position = 0;
                using (TcpClient client = new TcpClient()) {
                    var pending = client.BeginConnect(host, port, null, null);
                    using (var handle = pending.AsyncWaitHandle) {
                        if (!handle.WaitOne(15000)) throw new IOException("Connection timed out. Check receiver address and firewall.");
                        client.EndConnect(pending);
                    }
                    client.NoDelay = true;
                    using (NetworkStream net = client.GetStream()) {
                        net.ReadTimeout = Timeout; net.WriteTimeout = Timeout;
                        BinaryReader r = new BinaryReader(net, Utf8, true);
                        BinaryWriter w = new BinaryWriter(net, Utf8, true);
                        if (Get(r) != "LANFILE1") throw new IOException("Unsupported receiver protocol.");
                        string serverNonce = Get(r);
                        if (!ValidHash(serverNonce)) throw new IOException("Invalid server challenge.");
                        string clientNonce = Nonce();
                        Put(w, clientNonce);
                        string challenge = serverNonce + ":" + clientNonce;
                        Put(w, Proof(secret, "client", challenge));
                        if (!Equal(Get(r), Proof(secret, "server", challenge))) throw new IOException("Authentication failed. Check shared secret.");
                        Put(w, name); w.Write(file.Length); Put(w, hash);
                        string status = Get(r);
                        if (status == "CHECKING") { do { status = Get(r); } while (status == "VERIFYING"); }
                        if (status == "DONE") { Console.WriteLine("Already received and verified: " + name); return; }
                        if (status != "READY") throw new IOException(status);
                        long offset = r.ReadInt64();
                        if (offset < 0 || offset > file.Length) throw new IOException("Invalid resume offset.");
                        file.Position = offset;
                        Console.WriteLine("Sending {0}; resuming at {1:N1} MiB", name, offset / 1048576.0);
                        byte[] buffer = new byte[BufferSize];
                        Stopwatch watch = Stopwatch.StartNew(); long last = 0;
                        while (file.Position < file.Length) {
                            int count = file.Read(buffer, 0, (int)Math.Min(buffer.Length, file.Length - file.Position));
                            if (count == 0) throw new EndOfStreamException("Source ended unexpectedly.");
                            net.Write(buffer, 0, count);
                            if (watch.ElapsedMilliseconds - last >= 2000) {
                                Progress(file.Position, file.Length, watch, offset); last = watch.ElapsedMilliseconds;
                            }
                        }
                        Progress(file.Position, file.Length, watch, offset);
                        Console.WriteLine("Waiting for receiver SHA-256 verification...");
                        // Receiver sends periodic heartbeats while hashing large files.
                        do { status = Get(r); } while (status == "VERIFYING");
                        if (status != "DONE") throw new IOException(status);
                        Console.WriteLine("Success: receiver verified SHA-256 " + hash);
                    }
                }
            }
        }
        static string Verify(Stream file, BinaryWriter writer) {
            using (SHA256 sha = SHA256.Create()) {
                byte[] buffer = new byte[BufferSize]; int count;
                Stopwatch heartbeat = Stopwatch.StartNew();
                Put(writer, "VERIFYING");
                while ((count = file.Read(buffer, 0, buffer.Length)) > 0) {
                    sha.TransformBlock(buffer, 0, count, buffer, 0);
                    if (heartbeat.ElapsedMilliseconds > 10000) { Put(writer, "VERIFYING"); heartbeat.Restart(); }
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                return Hex(sha.Hash);
            }
        }
        static void ReceiveOne(TcpClient client, string directory, string parts, string secret) {
            using (NetworkStream net = client.GetStream()) {
                net.ReadTimeout = Timeout; net.WriteTimeout = Timeout;
                BinaryReader r = new BinaryReader(net, Utf8, true);
                BinaryWriter w = new BinaryWriter(net, Utf8, true);
                try {
                    Put(w, "LANFILE1");
                    string serverNonce = Nonce(); Put(w, serverNonce);
                    string clientNonce = Get(r);
                    if (!ValidHash(clientNonce)) throw new IOException("Invalid client challenge.");
                    string challenge = serverNonce + ":" + clientNonce;
                    if (!Equal(Get(r), Proof(secret, "client", challenge))) throw new IOException("Authentication failed.");
                    Put(w, Proof(secret, "server", challenge));
                    string name = Get(r); CheckName(name);
                    long length = r.ReadInt64(); string hash = Get(r);
                    if (length < 0 || !ValidHash(hash)) throw new IOException("Invalid file metadata.");
                    string destination = Path.Combine(directory, name);
                    if (File.Exists(destination)) {
                        // Avoid following links and never overwrite existing output files.
                        if ((File.GetAttributes(destination) & FileAttributes.ReparsePoint) != 0)
                            throw new IOException("Destination is a symbolic link.");
                        using (FileStream existing = new FileStream(destination, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                            if (existing.Length != length) throw new IOException("Destination already exists. Use a different -Name.");
                            // Sender is still awaiting READY, so use a separate preflight status.
                            Put(w, "CHECKING");
                            if (Verify(existing, w) != hash) throw new IOException("Destination already exists with different contents. Use a different -Name.");
                        }
                        Put(w, "DONE"); Console.WriteLine("Already verified: " + destination); return;
                    }
                    string key;
                    using (MemoryStream bytes = new MemoryStream(Utf8.GetBytes(name + ":" + length + ":" + hash))) { key = Hash(bytes); }
                    string partial = Path.Combine(parts, key + ".part");
                    if (File.Exists(partial) && (File.GetAttributes(partial) & FileAttributes.ReparsePoint) != 0)
                        throw new IOException("Partial file must not be a symbolic link.");
                    bool matches;
                    using (FileStream output = new FileStream(partial, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, BufferSize, FileOptions.SequentialScan)) {
                        if (output.Length > length) throw new IOException("Partial file is larger than source. Remove " + partial);
                        long offset = output.Length; output.Position = offset;
                        Put(w, "READY"); w.Write(offset); w.Flush();
                        Console.WriteLine("Receiving {0} ({1:N1} MiB), starting at {2:N1} MiB", name, length / 1048576.0, offset / 1048576.0);
                        byte[] buffer = new byte[BufferSize]; long remaining = length - offset;
                        Stopwatch watch = Stopwatch.StartNew(); long last = 0;
                        while (remaining > 0) {
                            int count = net.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                            if (count == 0) throw new EndOfStreamException("Sender disconnected. Rerun sender to resume.");
                            output.Write(buffer, 0, count); remaining -= count;
                            if (watch.ElapsedMilliseconds - last >= 2000) {
                                Progress(length - remaining, length, watch, offset); last = watch.ElapsedMilliseconds;
                            }
                        }
                        output.Flush(true); output.Position = 0;
                        Console.WriteLine("Verifying " + name + "...");
                        matches = Verify(output, w) == hash;
                    }
                    if (!matches) {
                        File.Delete(partial);
                        throw new IOException("SHA-256 mismatch. Partial file removed. Rerun sender to restart.");
                    }
                    File.Move(partial, destination);
                    Put(w, "DONE"); Console.WriteLine("Saved and verified: " + destination);
                } catch (Exception error) {
                    try { Put(w, "ERROR: " + error.Message); } catch { }
                    throw;
                }
            }
        }
        public static void Receive(string directory, string address, int port, string secret, bool once) {
            directory = Path.GetFullPath(directory); CheckDirectory(directory);
            string parts = Path.Combine(directory, ".lan-transfer"); CheckDirectory(parts);
            // Prevent two receivers from writing the same partial files.
            using (FileStream gate = new FileStream(Path.Combine(parts, "receiver.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                TcpListener listener = new TcpListener(IPAddress.Parse(address), port);
                listener.Server.ExclusiveAddressUse = true;
                listener.Start();
                Console.WriteLine("Listening on {0}:{1}; saving to {2}", address, port, directory);
                Console.WriteLine("Close this PowerShell window to stop. Files in progress can be resumed.");
                try {
                    do {
                        using (TcpClient client = listener.AcceptTcpClient()) {
                            Console.WriteLine("Connection from " + client.Client.RemoteEndPoint);
                            try { ReceiveOne(client, directory, parts, secret); }
                            catch (Exception error) {
                                if (once) throw;
                                Console.Error.WriteLine("Transfer failed: " + error.Message);
                            }
                        }
                    } while (!once);
                } finally { listener.Stop(); }
            }
        }
    }
}
'@
}
