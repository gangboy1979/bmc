--- .pristine/libifc-1.98.0-src/data/webifc_auth.c Tue Sep  4 18:17:33 2012
+++ source/libifc-1.98.0-src/data/webifc_auth.c Fri Sep  7 14:32:34 2012
@@ -53,7 +53,8 @@
 		JSONFIELD(PORTNUM,JSONFIELD_TYPE_INT16U),
 		JSONFIELD(IP,JSONFIELD_TYPE_STR),
 		JSONFIELD(BINDDN,JSONFIELD_TYPE_STR),
-		JSONFIELD(SEARCHBASE,JSONFIELD_TYPE_STR)
+		JSONFIELD(SEARCHBASE,JSONFIELD_TYPE_STR),
+		JSONFIELD(ATTRIBUTEOFUSERLOGIN,JSONFIELD_TYPE_STR)
 	};
 	WEBPAGE_WRITE_BEGIN();
 	WEBPAGE_WRITE_JSON_BEGIN(GETLDAPCFG);
@@ -69,7 +70,8 @@
 		LDAPCfg.PortNum,
 		LDAPCfg.IPAddr,
 		LDAPCfg.BindDN,
-		LDAPCfg.SearchBase);
+		LDAPCfg.SearchBase,
+		LDAPCfg.AttributeOfUserLogin);
 
 error_out:
 	WEBPAGE_WRITE_JSON_END(GETLDAPCFG,retval);
@@ -90,6 +92,7 @@
 	WP_VAR_DECLARE(BINDDN,char*);
 	WP_VAR_DECLARE(PASSWORD,char*);
 	WP_VAR_DECLARE(SEARCHBASE,char*);
+	WP_VAR_DECLARE(ATTRIBUTEOFUSERLOGIN,char*);
 
 	WEBPAGE_WRITE_BEGIN();
 	WEBPAGE_WRITE_JSON_BEGIN(SETLDAPCFG);
@@ -102,13 +105,14 @@
 	BINDDN = WP_GET_VAR_STR(BINDDN);
 	PASSWORD = WP_GET_VAR_STR(PASSWORD);
 	SEARCHBASE = WP_GET_VAR_STR(SEARCHBASE);
-
+	ATTRIBUTEOFUSERLOGIN = WP_GET_VAR_STR(ATTRIBUTEOFUSERLOGIN);
 	LDAPCfg.Enable = ENABLE;
 	LDAPCfg.PortNum = PORTNUM;
 	strcpy((char *)LDAPCfg.IPAddr,IP);
 	strcpy((char *)LDAPCfg.BindDN, BINDDN);
 	strcpy((char *)LDAPCfg.Password, PASSWORD);
 	strcpy((char *)LDAPCfg.SearchBase,SEARCHBASE);
+	strcpy((char *)LDAPCfg.AttributeOfUserLogin,ATTRIBUTEOFUSERLOGIN);
 
 	retval = setldapconfig(&LDAPCfg);
 	if(retval != RPC_HAPI_SUCCESS)
