package com.example.myvideoapp;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;

public class MainActivity extends AppCompatActivity {
    
    static {
        System.loadLibrary("video-transcoder");
    }
    
    public native String stringFromJNI();
    public native int transcodeVideo(String inputPath, String outputPath);
    
    private static final int PERMISSION_REQUEST = 1;
    private static final int FILE_SELECT = 2;
    
    private Button btnSelect, btnTranscode;
    private TextView tvStatus;
    private String inputFile = null;
    private String outputFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        btnSelect = findViewById(R.id.btn_select);
        btnTranscode = findViewById(R.id.btn_transcode);
        tvStatus = findViewById(R.id.tv_status);
        
        File inputDir = getExternalFilesDir(null);
        if (inputDir != null && !inputDir.exists()) inputDir.mkdirs();
        
        File dcimDir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM), "Tx_video");
        if (!dcimDir.exists()) dcimDir.mkdirs();
        outputFile = new File(dcimDir, "TX_VideoT_" + System.currentTimeMillis() + ".mp4").getAbsolutePath();
        
        checkPermission();
        
        btnSelect.setOnClickListener(v -> selectVideo());
        btnTranscode.setOnClickListener(v -> startTranscode());
    }
    
    private void checkPermission() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            String[] perms = {
                Manifest.permission.READ_EXTERNAL_STORAGE, 
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            };
            boolean needRequest = false;
            for (String p : perms) {
                if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED) {
                    needRequest = true;
                    break;
                }
            }
            if (needRequest) {
                ActivityCompat.requestPermissions(this, perms, PERMISSION_REQUEST);
            }
        }
    }
    
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST) {
            for (int i = 0; i < permissions.length; i++) {
                if (grantResults[i] != PackageManager.PERMISSION_GRANTED) {
                    Toast.makeText(this, "需要存储权限才能使用", Toast.LENGTH_LONG).show();
                    return;
                }
            }
        }
    }
    
    private void selectVideo() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("video/*");
        startActivityForResult(intent, FILE_SELECT);
    }
    
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        
        if (requestCode == FILE_SELECT && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            tvStatus.setText("正在复制文件...");
            btnSelect.setEnabled(false);
            
            new Thread(() -> {
                String path = copyToLocal(uri);
                runOnUiThread(() -> {
                    btnSelect.setEnabled(true);
                    if (path != null) {
                        inputFile = path;
                        tvStatus.setText("已选择: " + inputFile);
                        btnTranscode.setEnabled(true);
                        File f = new File(inputFile);
                        tvStatus.append("\n文件大小: " + (f.length() / 1024 / 1024) + " MB");
                    } else {
                        tvStatus.setText("文件复制失败，请检查权限");
                        btnTranscode.setEnabled(false);
                    }
                });
            }).start();
        }
    }
    
    private String copyToLocal(Uri uri) {
        if (uri == null) {
            Log.e("Transcoder", "URI 为 null");
            return null;
        }
        
        File localFile = new File(getExternalFilesDir(null), "input.mp4");
        
        try (java.io.InputStream is = getContentResolver().openInputStream(uri);
             java.io.FileOutputStream fos = new java.io.FileOutputStream(localFile)) {
            
            if (is == null) {
                Log.e("Transcoder", "无法打开输入流，权限被拒绝？");
                return null;
            }
            
            byte[] buf = new byte[8192];
            int len;
            long total = 0;
            
            while ((len = is.read(buf)) > 0) {
                fos.write(buf, 0, len);
                total += len;
            }
            
            Log.i("Transcoder", "复制完成: " + total + " bytes -> " + localFile.getAbsolutePath());
            
            if (localFile.exists() && localFile.length() > 0) {
                return localFile.getAbsolutePath();
            } else {
                Log.e("Transcoder", "文件写入后不存在或大小为0");
                return null;
            }
            
        } catch (Exception e) {
            Log.e("Transcoder", "复制失败: " + e.getMessage(), e);
            if (localFile.exists()) {
                localFile.delete();
            }
            return null;
        }
    }
    
    private void startTranscode() {
        if (inputFile == null) {
            Toast.makeText(this, "请先选择视频", Toast.LENGTH_SHORT).show();
            return;
        }
        
        if (!new File(inputFile).exists()) {
            tvStatus.setText("错误：输入文件不存在");
            return;
        }
        
        btnTranscode.setEnabled(false);
        tvStatus.setText("转码中...\n输入: " + inputFile + "\n输出: " + outputFile);
        
        new Thread(() -> {
            int result = transcodeVideo(inputFile, outputFile);
            
            File tempInput = new File(inputFile);
            long tempSize = tempInput.length(); // 先获取大小再删除
            if (tempInput.exists()) {
                boolean deleted = tempInput.delete();
                Log.i("Transcoder", "临时输入文件已删除: " + deleted + ", 释放了 " + (tempSize/1024/1024) + " MB");
            }
            
            runOnUiThread(() -> {
                btnTranscode.setEnabled(true);
                if (result == 0) {
                    File outFile = new File(outputFile);
                    if (outFile.exists()) {
                        tvStatus.setText("✅ 转码成功！\n" +
                                       "输出: " + outputFile + 
                                       "\n输出大小: " + (outFile.length() / 1024 / 1024) + " MB" +
                                       "\n已清理临时文件: " + (tempSize/1024/1024) + " MB");
                        
                        Intent intent = new Intent(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE);
                        intent.setData(android.net.Uri.fromFile(outFile));
                        sendBroadcast(intent);
                    } else {
                        tvStatus.setText("❌ 输出文件未生成");
                    }
                } else {
                    tvStatus.setText("❌ 转码失败，错误码: " + result);
                }
            });
        }).start();
    }
}
