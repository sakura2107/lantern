package org.lantern.client;
import android.media.*;
import java.io.*;
import java.nio.*;

/** Platform codec owns its buffers; output is bounded to one 10ms stereo frame per input. Single owner. */
final class OpusAudioDecoder implements AutoCloseable {
    static final int MAX_OUTPUT_CHUNK = 1920, MAX_OUTPUT_BYTES = 3840;
    final MediaCodec codec;
    final MediaCodec.BufferInfo outputInfo = new MediaCodec.BufferInfo();
    long hostOrigin=-1,decodedFrames,outputHostPts;
    static boolean available() {
        try {
            return new MediaCodecList(MediaCodecList.REGULAR_CODECS)
                       .findDecoderForFormat(MediaFormat.createAudioFormat("audio/opus", 48000, 2)) != null;
        } catch (Exception e) {
            return false;
        }
    }
    OpusAudioDecoder(int lookahead) throws IOException {
        codec = MediaCodec.createDecoderByType("audio/opus");
        try {
            MediaFormat format = MediaFormat.createAudioFormat("audio/opus", 48000, 2);
            ByteBuffer head = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN);
            head.put(new byte[] {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2})
                .putShort((short)lookahead)
                .putInt(48000)
                .putShort((short)0)
                .put((byte)0);
            head.flip();
            format.setByteBuffer("csd-0", head);
            format.setByteBuffer("csd-1", longBuffer(lookahead * 1_000_000_000L / 48000));
            format.setByteBuffer("csd-2", longBuffer(80_000_000));
            codec.configure(format, null, null, 0);
            codec.start();
        } catch (Exception error) {
            codec.release();
            throw new IOException("Opus 解码器初始化失败", error);
        }
    }
    static ByteBuffer longBuffer(long value) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder());
        b.putLong(value).flip();
        return b;
    }
    static int appendOutput(ByteBuffer out, int offset, int size, byte[] scratch, int validBytes)
        throws IOException {
        if (scratch == null || scratch.length < MAX_OUTPUT_BYTES)
            throw new IOException("Opus 解码缓冲不足");
        if (size < 0 || size > MAX_OUTPUT_CHUNK || validBytes < 0 || validBytes + size > MAX_OUTPUT_BYTES)
            throw new IOException("Opus 输出缓冲超限");
        if (size == 0)
            return validBytes;
        if (out == null || offset < 0)
            throw new IOException("Opus 输出缓冲异常");
        out.position(offset);
        out.get(scratch, validBytes, size);
        return validBytes + size;
    }
    int decodeInto(byte[] encoded, long pts, byte[] scratch) throws IOException {
        if (scratch == null || scratch.length < MAX_OUTPUT_BYTES)
            throw new IOException("Opus 解码缓冲不足");
        if (hostOrigin < 0)
            hostOrigin = pts;
        outputHostPts = hostOrigin + decodedFrames * 1_000_000L / 48000;
        int input = codec.dequeueInputBuffer(20000);
        if (input < 0)
            throw new IOException("Opus 输入缓冲超时");
        ByteBuffer buffer = codec.getInputBuffer(input);
        buffer.clear();
        buffer.put(encoded);
        codec.queueInputBuffer(input, 0, encoded.length, pts, 0);
        int validBytes = 0;
        for (int i = 0; i < 5; i++) {
            int output = codec.dequeueOutputBuffer(outputInfo, i == 0 ? 20000 : 0);
            if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat format = codec.getOutputFormat();
                if (format.getInteger(MediaFormat.KEY_CHANNEL_COUNT) != 2 ||
                    format.getInteger(MediaFormat.KEY_SAMPLE_RATE) != 48000)
                    throw new IOException("Opus 输出格式变化");
                continue;
            }
            if (output < 0)
                break;
            try {
                ByteBuffer out = outputInfo.size > 0 ? codec.getOutputBuffer(output) : null;
                validBytes = appendOutput(out, outputInfo.offset, outputInfo.size, scratch, validBytes);
            } finally {
                codec.releaseOutputBuffer(output, false);
            }
        }
        decodedFrames += validBytes / 4;
        return validBytes;
    }
    public void close() {
        try {
            codec.stop();
        } finally {
            codec.release();
        }
    }
}
