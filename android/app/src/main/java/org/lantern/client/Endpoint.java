package org.lantern.client;
import java.io.IOException;
final class Endpoint {
    final String host;final int port;
    private Endpoint(String h,int p){host=h;port=p;}
    static Endpoint parse(String value,int fallback)throws IOException{
        if(value==null||value.isEmpty()||value.length()>260||fallback<1||fallback>65535)throw new IOException("电脑地址无效");String host,port="";
        if(value.startsWith("[")){int end=value.indexOf(']');if(end<2)throw new IOException("IPv6地址方括号不完整");host=value.substring(1,end);if(end+1<value.length()){if(value.charAt(end+1)!=':')throw new IOException("IPv6端口分隔符错误");port=value.substring(end+2);if(port.isEmpty())throw new IOException("缺少端口");}}
        else{int colon=value.indexOf(':');if(colon>=0&&colon==value.lastIndexOf(':')){host=value.substring(0,colon);port=value.substring(colon+1);if(port.isEmpty())throw new IOException("缺少端口");}else host=value;}
        if(host.isEmpty()||host.length()>253)throw new IOException("电脑地址长度无效");for(char c:host.toCharArray())if(c<=32||c==127||c=='/'||c=='\\'||c=='['||c==']'||c=='@')throw new IOException("电脑地址含无效字符");
        int number=fallback;if(!port.isEmpty()){number=0;for(char c:port.toCharArray()){if(c<'0'||c>'9'||number>6553)throw new IOException("端口范围1..65535");number=number*10+c-'0';if(number>65535)throw new IOException("端口范围1..65535");}if(number==0)throw new IOException("端口范围1..65535");}return new Endpoint(host,number);
    }
    public String toString(){return (host.indexOf(':')<0?host:"["+host+"]")+":"+port;}
}
