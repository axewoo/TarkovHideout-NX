package io.github.axewoo.tarkovhideout;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

// The SDL activity running the shared C++ app (libmain.so), plus the HTTP client it calls,
// since Android has no system libcurl.
public class TarkovActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "SDL2_image", "SDL2_ttf", "main"};
    }

    private static void writeInt(ByteArrayOutputStream out, int v) {
        out.write(v >>> 24);
        out.write(v >>> 16);
        out.write(v >>> 8);
        out.write(v);
    }

    // Called from source/net.cpp: 4 bytes of HTTP status (-1 when the request failed), then the
    // body or the error message.
    public static byte[] httpGet(String url, String[] headers) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(30000);
            c.setInstanceFollowRedirects(true);
            for (String h : headers) {
                int i = h.indexOf(':');
                if (i > 0)
                    c.setRequestProperty(h.substring(0, i).trim(), h.substring(i + 1).trim());
            }
            int status = c.getResponseCode();
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            writeInt(out, status);
            if (in != null) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0)
                    out.write(buf, 0, n);
                in.close();
            }
        } catch (Exception e) {
            out.reset();
            writeInt(out, -1);
            byte[] msg = String.valueOf(e.getMessage() != null ? e.getMessage() : e.toString())
                                 .getBytes(StandardCharsets.UTF_8);
            out.write(msg, 0, msg.length);
        } finally {
            if (c != null)
                c.disconnect();
        }
        return out.toByteArray();
    }
}
