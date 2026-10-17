package org.lantern.client;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.charset.CodingErrorAction;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.security.*;
import java.security.cert.X509Certificate;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.net.ssl.*;

final class Remote implements AutoCloseable {
    enum PairFailure {
        INVALID_LINK,
        HOST_TOO_OLD,
        INVALID_INVITE,
        UNREACHABLE,
        PIN_MISMATCH,
        BUSY,
        PROTOCOL_ERROR,
        OUTCOME_UNKNOWN
    }
    static final class PairingException extends IOException {
        final PairFailure reason;
        PairingException(PairFailure r, String message) {
            super(message);
            reason = r;
        }
    }
    /** Blocking worker-thread API. Does not alter the existing HELLO/credential constructors. */
    static Credentials pair(PairingLink link, String deviceLabel) throws PairingException {
        if (link == null || link.expiresAt <= System.currentTimeMillis() / 1000 ||
            !PairingLink.validLabel(deviceLabel))
            throw new PairingException(PairFailure.INVALID_LINK,
                                       "配对链接无效或已过期，或设备名称超过64字节");
        List<ConnectionEndpoint> endpoints = link.endpoints;
        NetworkDialer.Result connected;
        try {
            connected = NetworkDialer.connect(endpoints, link.pin, 10000, null);
        } catch (NetworkDialer.ConnectException error) {
            throw new PairingException(error.reason == NetworkDialer.Failure.PIN_MISMATCH
                                           ? PairFailure.PIN_MISMATCH
                                           : PairFailure.UNREACHABLE,
                                       error.reason == NetworkDialer.Failure.PIN_MISMATCH
                                           ? "主机证书指纹不匹配，请核对二维码"
                                           : "所有地址均无法连接，请检查网络和防火墙");
        } catch (Exception error) {
            throw new PairingException(PairFailure.UNREACHABLE, "无法连接主机");
        }
        java.net.Socket raw = connected.transport;
        SSLSocket tls = connected.tls;
        boolean sent = false;
        try {
            DataOutputStream out = new DataOutputStream(tls.getOutputStream());
            byte[] packet = body(o -> {
                o.writeShort(1);
                o.writeShort(55);
                str(o, link.code);
                str(o, deviceLabel);
            });
            sent = true;
            out.writeInt(packet.length);
            out.write(packet);
            out.flush();
            DataInputStream input = new DataInputStream(tls.getInputStream());
            int count = input.readInt();
            if (count < 8 || count > 2048)
                throw new PairingException(PairFailure.PROTOCOL_ERROR, "主机配对响应无效");
            byte[] bytes = new byte[count];
            input.readFully(bytes);
            DataInputStream reply = reader(bytes);
            int version = reply.readUnsignedShort(), op = reply.readUnsignedShort(), error = reply.readInt();
            if (error == 6 && (op == 55 || op == 1))
                throw new PairingException(PairFailure.HOST_TOO_OLD, "主机版本过旧，请更新电脑端");
            if (version != 1 || op != 55)
                throw new PairingException(PairFailure.PROTOCOL_ERROR, "主机配对响应无效");
            if (error != 0) {
                str(reply);
                if (reply.available() != 0)
                    throw new IOException();
                if (error == 2)
                    throw new PairingException(PairFailure.INVALID_INVITE,
                                               "配对码无效或已过期，请在电脑刷新二维码");
                if (error == 8)
                    throw new PairingException(PairFailure.BUSY, "配对尝试过于频繁或设备已满，请稍后重试");
                throw new PairingException(PairFailure.PROTOCOL_ERROR, "主机拒绝配对请求");
            }
            String id = str(reply), token = str(reply);
            int scopes = reply.readInt();
            if (!id.matches("[0-9a-f]{64}") || !token.matches("[0-9a-f]{64}") || scopes < 1 || scopes > 31 ||
                reply.available() != 0)
                throw new PairingException(PairFailure.PROTOCOL_ERROR, "主机配对响应无效");
            return new Credentials(connected.selected.host, connected.selected.port, link.pin, token,
                                   endpoints);
        } catch (PairingException error) {
            throw error;
        } catch (Exception error) {
            if (sent)
                throw new PairingException(PairFailure.OUTCOME_UNKNOWN,
                                           "配对响应中断，结果无法确认；请检查电脑设备列表并重新生成邀请");
            throw new PairingException(PairFailure.PROTOCOL_ERROR, "配对请求无法完成");
        } finally {
            try {
                raw.close();
            } catch (IOException ignored) {
            }
            if (tls != null)
                try {
                    tls.close();
                } catch (IOException ignored) {
                }
        }
    }
    static final class TransportError extends IOException {
        TransportError(IOException cause){super("连接中断："+cause.getClass().getSimpleName(),cause);}
    }
    static final class RemoteError extends IOException {
        final int code;
        RemoteError(int value,String message) {super(message);code=value;}
    }
    static final class Credentials {
        final String host, pin, token;
        final int port;
        /** True only for the connection the user starts with “连接电脑”; everything else is automatic. */
        final boolean deliberate;
        final List<ConnectionEndpoint> endpoints;
        Credentials(String h, String p, String t) {
            this(h, 47443, p, t);
        }
        Credentials(String h, int number, String p, String t) { this(h, number, p, t, false); }
        private Credentials(String h, int number, String p, String t, boolean d) {
            this(h, number, p, t, d, Collections.emptyList());
        }
        Credentials(String h, int number, String p, String t, List<ConnectionEndpoint> list) {
            this(h, number, p, t, false, list);
        }
        private Credentials(String h, int number, String p, String t, boolean d,
                            List<ConnectionEndpoint> list) {
            host = h;
            port = number;
            pin = p;
            token = t;
            deliberate = d;
            endpoints =
                ConnectionEndpoint.merge(Collections.emptyList(), list, System.currentTimeMillis() / 1000);
        }
        Credentials asDeliberate() {
            return new Credentials(host, port, pin, token, true, endpoints);
        }
        Credentials mergeEndpoints(List<ConnectionEndpoint> fresh) {
            return new Credentials(
                host, port, pin, token, deliberate,
                ConnectionEndpoint.merge(endpoints, fresh, System.currentTimeMillis() / 1000));
        }
        String saveEndpointPreferences() {
            return ConnectionEndpoint.savePreferences(endpoints);
        }
        Credentials loadEndpointPreferences(String value) {
            return new Credentials(host, port, pin, token, deliberate,
                                   ConnectionEndpoint.loadPreferences(value));
        }
        List<ConnectionEndpoint> candidates() {
            return endpoints.isEmpty() ? Collections.singletonList(ConnectionEndpoint.of(host, port))
                                       : endpoints;
        }
    }
    /** Host (by certificate pin) that advertised session=end-v1, i.e. accepts the HELLO flags field. */
    static volatile String sessionEndPin;
    static final int HELLO_AUTOMATIC = 1;
    /** The host ended this device's connections from its management window (session=end-v1). */
    static final String ENDED_BY_HOST = "电脑已结束此设备的连接";
    static boolean endedByHost(Throwable error) {
        for (int depth = 0; error != null && depth < 16; depth++, error = error.getCause())
            if (error instanceof RemoteError && ((RemoteError)error).code == 7 && error.getMessage() != null &&
                error.getMessage().contains(ENDED_BY_HOST))
                return true;
        return false;
    }
    static final class Entry extends FileItem {
        Entry(String n, boolean d, long s, long m) {
            super(n,d,s,m);
        }
    }
    static final class Info {
        final long size;
        final String version, hash;
        Info(long s, String v, String h) {
            size = s;
            version = v;
            hash = h;
        }
    }
    static final int CHUNK = 262144, MAX = 4194304;
    final java.net.Socket transport;
    final Credentials credentials;
    final int grantedScopes;
    final SSLSocket socket;
    final DataInputStream input;
    final DataOutputStream output;
    final ConnectionEndpoint selectedEndpoint;
    final boolean publicPath;
    final List<NetworkDialer.Probe> connectionAttempts;
    interface SocketReady { void opened(java.net.Socket transport) throws IOException; }
    Remote(Credentials c, int scopes, int lane) throws Exception { this(c, scopes, lane, null); }
    Remote(Credentials c, int scopes, int lane, SocketReady opening) throws Exception {
        credentials=c;grantedScopes=scopes;
        if (!c.pin.matches("[0-9a-f]{64}")) throw new IOException("请核对主机 SHA256 指纹");
        NetworkDialer.Result connected =
            c.endpoints.isEmpty() ? NetworkDialer.connectLegacy(c.host, c.port, c.pin, 10000, opening)
                                  : NetworkDialer.connect(c.candidates(), c.pin, 10000, opening);
        transport = connected.transport;
        selectedEndpoint = connected.selected;
        publicPath = connected.publicPath;
        connectionAttempts = connected.attempts;
        SSLSocket connecting = null;
        try {
            socket = connecting = connected.tls;
            input = new DataInputStream(new BufferedInputStream(socket.getInputStream(), 65536));
            output = new DataOutputStream(new BufferedOutputStream(socket.getOutputStream(), CHUNK + 1024));
            // The flags field is only sent to hosts that advertised session=end-v1; older hosts reject it.
            boolean flags = c.pin.equals(sessionEndPin);
            byte[] answer = call(1, body(o -> {
                str(o, c.token); o.writeInt(scopes); o.writeInt(lane);
                if (flags) o.writeInt(c.deliberate ? 0 : HELLO_AUTOMATIC);
            }));
            if (reader(answer).readInt() != scopes) throw new IOException("权限不匹配");
            socket.setSoTimeout(15000);
        } catch (Exception error) {
            transport.close();
            if (connecting != null) try { connecting.close(); } catch (IOException ignored) {}
            throw error;
        }
    }
    List<ConnectionEndpoint> endpoints() throws Exception {
        if (!str(reader(call(30, new byte[0]))).contains(";endpoints=v1"))
            return Collections.emptyList();
        DataInputStream r = reader(call(56, new byte[0]));
        if (r.readInt() != 1)
            throw new IOException("入口协议版本无效");
        List<ConnectionEndpoint> out = ConnectionEndpoint.read(r);
        if (r.available() != 0)
            throw new IOException("入口响应无效");
        return out;
    }
    static List<NetworkDialer.Probe> probeEndpoints(Credentials c) throws Exception {
        List<NetworkDialer.Probe> reports = new ArrayList<>();
        for (ConnectionEndpoint e : c.candidates()) {
            long start = System.nanoTime();
            try (Remote remote = new Remote(
                     new Credentials(e.host, e.port, c.pin, c.token, Collections.singletonList(e)), 0, 0)) {
                long begin = System.nanoTime(), nonce = begin;
                DataInputStream r = reader(remote.call(50, body(o -> o.writeLong(nonce))));
                if (r.readLong() != nonce || r.available() != 0)
                    throw new IOException("诊断响应无效");
                reports.add(new NetworkDialer.Probe(e, null, (begin - start) / 1000000, 0, true, true,
                                                    (System.nanoTime() - begin) / 1000000));
            } catch (NetworkDialer.ConnectException error) {
                reports.addAll(error.attempts);
            } catch (Exception error) {
                reports.add(new NetworkDialer.Probe(e, NetworkDialer.Failure.TRANSPORT,
                                                    (System.nanoTime() - start) / 1000000, 0, false, false,
                                                    null));
            }
        }
        return reports;
    }
    // Cancellation only closes the raw socket. No TLS close_notify writes on the UI thread.
    void abort() throws IOException { transport.close(); }
interface Write {
    void run(DataOutputStream o) throws Exception;
}
static byte[] body(Write w) throws Exception {
    ByteArrayOutputStream b = new ByteArrayOutputStream();
    DataOutputStream out = new DataOutputStream(b);
    w.run(out);
    return b.toByteArray();
}
static void str(DataOutputStream o, String s) throws IOException {
    if(s.length()>65536)throw new IOException("UTF-8字符串超限");
    ByteBuffer encoded=StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(s));
    if(encoded.remaining()>65536)throw new IOException("UTF-8字符串超限");byte[] b=new byte[encoded.remaining()];encoded.get(b);
    o.writeInt(b.length);o.write(b);
}
static byte[] blob(DataInputStream i, int max) throws IOException {
    int n = i.readInt();
    if (n < 0 || n > max)
        throw new IOException("协议字段越界");
    byte[] b = new byte[n];
    i.readFully(b);
    return b;
}
static String str(DataInputStream i) throws IOException {
    return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(blob(i,65536))).toString();
}
static DataInputStream reader(byte[] b) {
    return new DataInputStream(new ByteArrayInputStream(b));
}
synchronized byte[] call(int op, byte[] payload) throws IOException {
    byte[] b = Native.packet(op, payload);
    try{output.writeInt(b.length);output.write(b);output.flush();}catch(IOException error){throw new TransportError(error);}
    return receive(op);
}
synchronized void sendOnly(int op, byte[] payload) throws IOException {
    byte[] b = Native.packet(op, payload);
    output.writeInt(b.length);
    output.write(b);
    output.flush();
}
byte[] receive(int op) throws IOException {
    int n;try{n=input.readInt();}catch(IOException error){throw new TransportError(error);}
    if (n < 8 || n > MAX)
        throw new IOException("协议帧越界");
    byte[] response = new byte[n];
    try{input.readFully(response);}catch(IOException error){throw new TransportError(error);}
    try { return Native.response(op, response); }
    catch(IOException error) {
        DataInputStream header=reader(response);
        if(header.readUnsignedShort()==1 && header.readUnsignedShort()==op) {
            int code=header.readInt();
            if(code>0 && code<=10) throw new RemoteError(code,error.getMessage());
        }
        throw error;
    }
}

List<Entry> list(String path, int offset) throws Exception {
    var r = reader(call(2, body(o -> {
                            str(o, path);
                            o.writeInt(offset);
                            o.writeInt(256);
                            str(o, "");
                        })));
    str(r);
    int count = r.readInt();
    if (count < 0 || count > 256)
        throw new IOException("列表越界");
    List<Entry> out = new ArrayList<>();
    for (int i = 0; i < count; i++)
        out.add(new Entry(str(r), r.readInt() != 0, r.readLong(), r.readLong()));
    return out;
}
static final class Page {
    final String generation;final int total;final List<Entry> entries;
    Page(String g,int t,List<Entry> items){generation=g;total=t;entries=items;}
}
static List<Entry> entries(DataInputStream input,int bound) throws IOException {
    int count=input.readInt();if(count<0||count>bound)throw new IOException("文件页数量越界");
    List<Entry> items=new ArrayList<>();
    for(int i=0;i<count;i++){String name=str(input);int kind=input.readInt();long size=input.readLong(),modified=input.readLong();if(kind<0||kind>1||size<0)throw new IOException("文件元数据越界");items.add(new Entry(name,kind!=0,size,modified));}
    return items;
}
Page page(String path,int offset,String filter,int sort,boolean descending,String generation) throws Exception {
    DataInputStream input=reader(call(44,body(o -> {o.writeInt(1);str(o,path);o.writeInt(offset);o.writeInt(256);str(o,filter);o.writeInt(sort);o.writeInt(descending?1:0);str(o,generation);}))); 
    if(input.readInt()!=1)throw new IOException("文件页协议版本");String version=str(input);int total=input.readInt();List<Entry> items=entries(input,256);
    if(total<items.size()||total>200000||input.available()!=0)throw new IOException("文件页格式错误");return new Page(version,total,items);
}
static final class SearchPage {final boolean done;final long scanned;final List<Entry> entries;SearchPage(boolean d,long s,List<Entry> e){done=d;scanned=s;entries=e;}}
SearchPage search(int phase,String path,String query) throws Exception {
    DataInputStream input=reader(call(45,body(o -> {o.writeInt(1);o.writeInt(phase);if(phase==0){str(o,path);str(o,query);}})));
    if(input.readInt()!=1)throw new IOException("搜索协议版本");int done=input.readInt();long scanned=input.readLong();List<Entry> items=entries(input,128);
    if(done<0||done>1||scanned<0||scanned>200000||input.available()!=0)throw new IOException("搜索响应越界");return new SearchPage(done!=0,scanned,items);
}
byte[] thumbnail(String path) throws Exception {
    DataInputStream input=reader(call(46,body(o -> {o.writeInt(1);str(o,path);o.writeInt(128);})));if(input.readInt()!=1)throw new IOException("缩略图协议版本");byte[] image=blob(input,262144);if(input.available()!=0)throw new IOException("缩略图多余字段");return image;
}
void mutate(int op,String path,String destination) throws Exception {call(op,body(o -> {str(o,path);str(o,destination);}));}
String startJob(int kind,String path,String destination) throws Exception {
    DataInputStream input=reader(call(47,body(o -> {o.writeInt(1);o.writeInt(kind);str(o,path);str(o,destination);})));String id=str(input);
    if(!id.matches("[0-9a-f]{64}")||input.available()!=0)throw new IOException("主机任务标识错误");return id;
}
static final class HostJob {
    final int state,kind,error;final long current,total;final String message;final Info info;
    HostJob(int s,long c,long t,int e,String m,int k,Info f){state=s;current=c;total=t;error=e;message=m;kind=k;info=f;}
    boolean terminal(){return state>=4;}
}
HostJob pollJob(String id) throws Exception {
    DataInputStream input=reader(call(48,body(o -> {o.writeInt(1);str(o,id);})));int state=input.readInt();long current=input.readLong(),total=input.readLong();int error=input.readInt();String message=str(input);int kind=input.readInt();Info info=new Info(input.readLong(),str(input),str(input));
    if(state<0||state>6||current<0||total<0||kind<0||kind>2||error<0||error>10||input.available()!=0)throw new IOException("任务状态响应越界");return new HostJob(state,current,total,error,message,kind,info);
}
void cancelJob(String id) throws Exception {call(49,body(o -> {o.writeInt(1);str(o,id);}));}
Info hashSnapshot(String path,AtomicBoolean cancel,Progress progress) throws Exception {
    if(cancel.get())throw new IOException("已取消");String id=startJob(2,path,"");
    try {
        for(;;){if(cancel.get())throw new IOException("已取消");HostJob job=pollJob(id);progress.update(job.current,job.total);
            if(job.state==4){if(!job.info.hash.matches("[0-9a-f]{64}"))throw new IOException("主机校验结果错误");return job.info;}
            if(job.terminal())throw new RemoteError(job.error,job.message);Thread.sleep(25);}
    }catch(Exception error){if(cancel.get())try(Remote control=new Remote(credentials,1,0)){control.cancelJob(id);}catch(Exception ignored){}throw error;}
}
Info stat(String path, boolean hash) throws Exception {
    var r = reader(call(hash ? 3 : 31, body(o -> str(o, path))));
    Info info = new Info(r.readLong(), str(r), str(r));
    if (info.size < 0 || (hash && !info.hash.matches("[0-9a-f]{64}")))
        throw new IOException("文件元数据无效");
    return info;
}
byte[] read(String path, String version, long offset, int length) throws Exception {
    return blob(reader(call(4, body(o -> {
                                str(o, path);
                                str(o, version);
                                o.writeLong(offset);
                                o.writeInt(length);
                            }))),
                CHUNK);
}
static String hex(byte[] b) {
    StringBuilder s = new StringBuilder();
    for (byte v : b)
        s.append(String.format(Locale.ROOT, "%02x", v & 255));
    return s.toString();
}
static String hash(File f) throws Exception {return hash(f,new AtomicBoolean());}
static String hash(File f,AtomicBoolean cancel) throws Exception {
    MessageDigest d = MessageDigest.getInstance("SHA-256");
    try (InputStream i = new FileInputStream(f)) {
        byte[] b = new byte[CHUNK];
        int n;
        while ((n = i.read(b)) != -1) {
            if(cancel.get())throw new IOException("校验已暂停，暂存保留");
            d.update(b, 0, n);
        }
    }
    return hex(d.digest());
}
interface Progress {
    void update(long current, long total);
}
void download(String path, File dest, AtomicBoolean cancel, Progress progress) throws Exception {
    if (cancel.get())
        throw new IOException("已取消");
    Info f = hashSnapshot(path, cancel, (n,t) -> {});
    if (dest.exists()) {
        if (dest.length() == f.size && hash(dest,cancel).equals(f.hash)) {
            progress.update(f.size, f.size);
            return;
        }
        throw new IOException("本地文件版本不同，保留原件，请清理缓存或选择其他文件");
    }
    File part = new File(dest + ".part"), meta = new File(dest + ".resume");
    byte[] expected = body(o -> {
        str(o, path);
        o.writeLong(f.size);
        str(o, f.version);
        str(o, f.hash);
    });
    if (part.exists()) {
        if (!meta.exists() || meta.length() > 65536 ||
            !Arrays.equals(java.nio.file.Files.readAllBytes(meta.toPath()), expected))
            throw new IOException("恢复记录不属于当前源版本");
    } else
        java.nio.file.Files.write(meta.toPath(), expected);
    try (RandomAccessFile out = new RandomAccessFile(part, "rw")) {
        long offset = out.length();
        if (offset > f.size)
            throw new IOException("暂存文件大小错误");
        out.seek(offset);
        progress.update(offset, f.size);
        while (offset < f.size) {
            if (cancel.get())
                throw new IOException("已取消，进度已保留");
            TransferPacer.SHARED.acquire((int)Math.min(CHUNK, f.size-offset),cancel);
            byte[] b = read(path, f.version, offset, (int)Math.min(CHUNK, f.size - offset));
            if (b.length == 0)
                throw new IOException("源文件意外结束");
            out.write(b);
            out.getFD().sync();
            TransferPacer.SHARED.down.add(b.length);
            offset += b.length;
            progress.update(offset, f.size);
        }
    }
    if (cancel.get())
        throw new IOException("已取消，进度已保留");
    if (!hash(part,cancel).equals(f.hash))
        throw new IOException("SHA256 校验失败，保留暂存文件");
    java.nio.file.Files.move(part.toPath(), dest.toPath());
    meta.delete();
}
interface UploadSource extends AutoCloseable {
    long size();String hash(AtomicBoolean cancel)throws Exception;void seek(long offset)throws IOException;
    int read(byte[] buffer,int limit)throws IOException;void validate()throws IOException;void close()throws IOException;
}
static String sourceHash(UploadSource source,AtomicBoolean cancel)throws Exception{
    source.validate();source.seek(0);MessageDigest digest=MessageDigest.getInstance("SHA-256");byte[] buffer=new byte[CHUNK];long bytes=0;
    while(bytes<source.size()){if(cancel.get())throw new IOException("源文件校验已取消");int count=source.read(buffer,(int)Math.min(CHUNK,source.size()-bytes));if(count<=0)throw new IOException("源文件长度变化");digest.update(buffer,0,count);bytes+=count;}
    source.validate();return hex(digest.digest());
}
static final class FileSource implements UploadSource{
    final File path;final RandomAccessFile file;final long length,modified;
    FileSource(File p)throws IOException{if(!p.isFile())throw new IOException("上传源不是普通文件");path=p;length=p.length();modified=p.lastModified();file=new RandomAccessFile(p,"r");}
    public long size(){return length;}public String hash(AtomicBoolean cancel)throws Exception{return sourceHash(this,cancel);}
    public void seek(long at)throws IOException{file.seek(at);}public int read(byte[] buffer,int limit)throws IOException{return file.read(buffer,0,limit);}
    public void validate()throws IOException{if(file.length()!=length||!path.isFile()||path.length()!=length||path.lastModified()!=modified)throw new IOException("本地源文件变化");}
    public void close()throws IOException{file.close();}
}
void upload(File source,String path,AtomicBoolean cancel,Progress progress)throws Exception{try(UploadSource in=new FileSource(source)){upload(in,path,cancel,progress);}}
void upload(UploadSource source,String path,AtomicBoolean cancel,Progress progress)throws Exception{
    if(cancel.get())throw new IOException("已取消");long size=source.size();if(size<0)throw new IOException("上传源大小未知");String hash=source.hash(cancel);source.validate();
    try{Info existing=hashSnapshot(path,cancel,(n,t)->{});if(existing.size==size&&existing.hash.equals(hash)){source.validate();progress.update(size,size);return;}throw new RemoteError(4,"同名文件内容不同，保留电脑原件，请选择新的名称");}catch(RemoteError error){if(error.code!=3)throw error;}
    DataInputStream ack=reader(call(5,body(o->{str(o,path);o.writeLong(size);str(o,hash);})));String id=str(ack);long offset=ack.readLong();if(offset<0||offset>size||!id.matches("[0-9a-f]{64}")||ack.available()!=0)throw new IOException("上传恢复信息无效");
    source.seek(offset);byte[] buffer=new byte[CHUNK];progress.update(offset,size);
    while(offset<size){if(cancel.get())throw new IOException("已取消；远端暂存保留");int count=source.read(buffer,(int)Math.min(CHUNK,size-offset));if(count<=0)throw new IOException("源文件长度变化");TransferPacer.SHARED.acquire(count,cancel);long current=offset;
        DataInputStream answer=reader(call(6,body(o->{str(o,id);o.writeLong(current);o.writeInt(count);o.write(buffer,0,count);})));long next=answer.readLong();if(next!=offset+count||answer.available()!=0)throw new IOException("上传确认偏移错误");TransferPacer.SHARED.up.add(count);offset=next;progress.update(offset,size);
    }
    source.validate();if(cancel.get())throw new IOException("已取消，远端暂存已保留");call(7,body(o->str(o,id)));
}

@Override
public void close() throws IOException {
    socket.close();
}
}
