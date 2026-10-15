package org.lantern.client;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.*;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
final class SafValidation {
 static void run(DeviceTestRunner runner,MainActivity activity,Remote.Credentials credentials)throws Exception{
    runner.phase("Seekable SAF upload, resume, source change and fallback");
    File fixture=new File(activity.getFilesDir(),"saf-test-fixture");fixture.mkdirs();File file=new File(fixture,"direct.bin");byte[] data=new byte[2*1024*1024];for(int i=0;i<data.length;i++)data[i]=(byte)(i*179+i/313);java.nio.file.Files.write(file.toPath(),data);
    Uri uri=DocumentsContract.buildDocumentUri("org.lantern.client.testdocs","direct.bin");String hash=Remote.hash(file);AtomicBoolean cancel=new AtomicBoolean();boolean paused=false;
    try(Remote remote=new Remote(credentials,3,3);SafSource source=SafSource.open(activity.getContentResolver(),uri)){
        runner.check(source!=null&&source.size()==data.length,"real provider descriptor is seekable");
        try{remote.upload(source,"saf-direct.bin",cancel,(n,total)->{if(n>=512*1024)cancel.set(true);});}catch(IOException expected){paused=cancel.get();}
    }runner.check(paused,"direct source upload cancelled with stage retained");cancel.set(false);
    long[] first={-1};try(Remote remote=new Remote(credentials,3,3);SafSource source=SafSource.open(activity.getContentResolver(),uri)){
        remote.upload(source,"saf-direct.bin",cancel,(n,total)->{if(first[0]<0)first[0]=n;});runner.check(remote.stat("saf-direct.bin",true).hash.equals(hash),"direct source resume SHA256");
    }runner.check(first[0]==512*1024,"provider seeks to actual remote resume offset");
    boolean changed=false;try(Remote remote=new Remote(credentials,3,3);SafSource source=SafSource.open(activity.getContentResolver(),uri)){
        try{remote.upload(source,"saf-mutated.bin",cancel,(n,total)->{if(n==256*1024){try(RandomAccessFile writer=new RandomAccessFile(file,"rw")){writer.write(17);}catch(IOException e){throw new UncheckedIOException(e);}file.setLastModified(file.lastModified()+2000);}});}catch(IOException expected){changed=true;}
        boolean absent=false;try{remote.stat("saf-mutated.bin",false);}catch(Remote.RemoteError e){absent=e.code==3;}runner.check(absent,"mutated source never creates final destination");
    }runner.check(changed,"same-size provider mutation detected");java.nio.file.Files.write(file.toPath(),data);
    runner.runOnMainSync(()->activity.transfers.close());File area=new File(activity.getFilesDir(),"transfers/validation-saf-"+UUID.randomUUID());TransferQueue queue=new TransferQueue(activity,area);queue.credentials=credentials;queue.attach(activity);runner.runOnMainSync(()->activity.transfers=queue);TransferPacer.SHARED.userLimit=512*1024;
    runner.runOnMainSync(()->queue.add("saf-journal.bin","direct.bin",false,3,uri.toString()));TransferQueue.Task task=runner.latest(queue);runner.awaitBytes(task,512*1024);queue.pause(task,false);
    long end=System.nanoTime()+3_000_000_000L;while(queue.running!=null&&System.nanoTime()<end)Thread.sleep(10);runner.check(queue.running==null&&!task.sourceHash.isEmpty(),"pause closes source and persists hash");queue.close();
    TransferQueue restored=new TransferQueue(activity,area);restored.credentials=credentials;restored.attach(activity);runner.runOnMainSync(()->activity.transfers=restored);TransferQueue.Task loaded=runner.latest(restored);
    runner.check(loaded.kind==3&&loaded.local.equals(uri.toString())&&loaded.sourceHash.equals(task.sourceHash),"URI task survives journal reconstruction");restored.resume(loaded);runner.awaitTask(loaded,"完成",20000);
    try(Remote remote=new Remote(credentials,1,3)){runner.check(remote.stat("saf-journal.bin",true).hash.equals(hash),"restored URI task exact bytes");}
    File pipeFile=new File(fixture,"stream-only.bin");java.nio.file.Files.write(pipeFile.toPath(),new byte[]{1,2,3,4});Uri pipe=DocumentsContract.buildDocumentUri("org.lantern.client.testdocs","stream-only.bin");
    try(SafSource source=SafSource.open(activity.getContentResolver(),pipe)){runner.check(source==null,"pipe source requests snapshot fallback");}
    int before; synchronized(restored){before=restored.tasks.size();}runner.runOnMainSync(()->{activity.path.setText("");activity.stageUpload(pipe);});end=System.nanoTime()+8_000_000_000L;
    while(restored.tasks.size()==before&&System.nanoTime()<end)Thread.sleep(30);runner.check(restored.tasks.size()>before,"unseekable source queued through snapshot fallback");TransferQueue.Task fallback=runner.latest(restored);runner.awaitTask(fallback,"完成",8000);runner.check(fallback.kind==1,"fallback retains existing cached upload type");
    try(java.util.stream.Stream<java.nio.file.Path> paths=java.nio.file.Files.walk(area.toPath())){runner.check(paths.noneMatch(p->p.getFileName().toString().equals("source.snapshot")),"seekable sources create no full snapshot");}
    restored.close();runner.report.put("saf_streaming_zero_snapshot_verified",true);runner.report.put("source_bytes",data.length);runner.report.put("source_resume_offset",first[0]);runner.report.put("real_system_persistable_picker_grant_tested",false);
 }
}
