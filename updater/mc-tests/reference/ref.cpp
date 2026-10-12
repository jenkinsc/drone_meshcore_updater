// Reference vectors produced by MeshCore's own Utils/Identity code (host build).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <Utils.h>
#include <Identity.h>

static void hex(const char*l,const uint8_t*p,size_t n){printf("%s=",l);for(size_t i=0;i<n;i++)printf("%02x",p[i]);printf("\n");}

struct FixedRNG : public mesh::RNG {
  uint8_t base; FixedRNG(uint8_t b):base(b){}
  void random(uint8_t* dest, size_t sz) override { for(size_t i=0;i<sz;i++) dest[i]=(uint8_t)(base+i*7+3); }
};

int main(){
  FixedRNG ra(0x10), rb(0x55);
  mesh::LocalIdentity node(&ra), op(&rb);
  uint8_t secret_n[32], secret_o[32];
  node.calcSharedSecret(secret_n, op.pub_key);   // node's view of the shared secret
  op.calcSharedSecret(secret_o, node.pub_key);   // operator's view
  hex("node_pub", node.pub_key, 32);
  hex("op_pub", op.pub_key, 32);
  hex("secret_node_side", secret_n, 32);
  hex("secret_op_side", secret_o, 32);

  // operator -> node text message plaintext: ts(4 LE) flags(1) text, as BaseChatMesh::composeMsgPacket
  const char* text = "arm EA:7C:11:0B:55:90 fw.zip";
  uint8_t plain[5+64]; uint32_t ts = 1760000000;
  memcpy(plain,&ts,4); plain[4]=0; memcpy(&plain[5],text,strlen(text)+1);
  int plen = 5+strlen(text);
  hex("plain", plain, plen);
  uint8_t sealed[200];
  int slen = mesh::Utils::encryptThenMAC(secret_o, sealed, plain, plen);
  hex("sealed", sealed, slen);

  // decrypt with node's side and show it round-trips
  uint8_t dec[200]; int dl = mesh::Utils::MACThenDecrypt(secret_n, dec, sealed, slen);
  printf("decrypt_len=%d\n", dl); hex("decrypted", dec, dl);

  // ack hash the node must send: sha256(data[0..5+text_len) , sender pub)[:4]
  uint8_t ack[4]; mesh::Utils::sha256(ack, 4, plain, plen, op.pub_key, 32);
  hex("ack4", ack, 4);

  // tamper test: flip a bit => must fail
  sealed[5] ^= 1; int bad = mesh::Utils::MACThenDecrypt(secret_n, dec, sealed, slen);
  printf("tampered_len=%d\n", bad);

  // signature over an advert body, node signs
  uint8_t msg[32+4+3]; memcpy(msg,node.pub_key,32); uint32_t ats=1760000100; memcpy(&msg[32],&ats,4);
  msg[36]=0x81; msg[37]='D'; msg[38]='r';
  uint8_t sig[64]; node.sign(sig,msg,39); hex("advert_msg", msg, 39); hex("advert_sig", sig, 64);
  printf("verify=%d\n", (int)node.verify(sig,msg,39));
  return 0;
}
