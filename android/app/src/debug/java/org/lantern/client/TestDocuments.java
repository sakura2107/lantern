package org.lantern.client;
import android.database.*;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import java.io.*;

/** Debug-only provider exposes only app-generated fixtures, never shared storage. */
public final class TestDocuments extends DocumentsProvider {
    File root(){return new File(getContext().getFilesDir(),"saf-test-fixture");}
    File file(String id)throws FileNotFoundException{try{File f=id.equals("root")?root():new File(root(),id);if(!f.getCanonicalPath().equals(root().getCanonicalPath())&&!f.getCanonicalPath().startsWith(root().getCanonicalPath()+File.separator))throw new IOException();return f;}catch(IOException e){throw new FileNotFoundException("Invalid test id");}}
    public boolean onCreate(){return true;}
    public Cursor queryRoots(String[] projection){return new MatrixCursor(projection==null?new String[]{"root_id","document_id","title","flags"}:projection);}
    MatrixCursor cursor(String[] projection){return new MatrixCursor(projection==null?TreeUpload.COLUMNS:projection);}
    void add(MatrixCursor c,String id,File f){MatrixCursor.RowBuilder row=c.newRow();for(String column:c.getColumnNames()){Object value=null;switch(column){case "document_id":value=id;break;case "_display_name":value=f.getName();break;case "mime_type":value=f.isDirectory()?DocumentsContract.Document.MIME_TYPE_DIR:"application/octet-stream";break;case "_size":value=f.length();break;case "last_modified":value=f.lastModified();break;}row.add(column,value);}}
    public Cursor queryDocument(String id,String[] projection)throws FileNotFoundException{MatrixCursor c=cursor(projection);File f=file(id);if(!f.exists())throw new FileNotFoundException();add(c,id,f);return c;}
    public Cursor queryChildDocuments(String id,String[] projection,String order)throws FileNotFoundException{MatrixCursor c=cursor(projection);File[] children=file(id).listFiles();if(children==null)throw new FileNotFoundException();for(File f:children)add(c,id.equals("root")?f.getName():id+"/"+f.getName(),f);return c;}
    public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal signal)throws FileNotFoundException{if(!mode.equals("r"))throw new FileNotFoundException("Read only fixture");if(id.equals("stream-only.bin")){try{ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe();File source=file(id);new Thread(()->{try(OutputStream out=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1]);InputStream in=new FileInputStream(source)){in.transferTo(out);}catch(IOException ignored){}},"Generated pipe fixture").start();return pipe[0];}catch(IOException e){throw new FileNotFoundException("Pipe fixture unavailable");}}return ParcelFileDescriptor.open(file(id),ParcelFileDescriptor.MODE_READ_ONLY);}
    public boolean isChildDocument(String parent,String child){return parent.equals("root")||child.startsWith(parent+"/");}
}
