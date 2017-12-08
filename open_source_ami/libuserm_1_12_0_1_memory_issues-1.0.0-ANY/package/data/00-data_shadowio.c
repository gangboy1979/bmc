--- .pristine/libuserm-1.12.0-src/data/shadowio.c Wed Sep 14 14:28:24 2011
+++ source/libuserm-1.12.0-src/data/shadowio.c Wed Sep 14 15:42:13 2011
@@ -36,10 +36,13 @@
 		return NULL;
 	*sp = *spent;
 	if (!(sp->sp_namp = strdup(spent->sp_namp)))
-		return NULL;
+		goto error_out;
 	if (!(sp->sp_pwdp = strdup(spent->sp_pwdp)))
-		return NULL;
+		goto error_out;
 	return sp;
+error_out:
+	free(sp);
+	return NULL;
 }
 
 static void *
