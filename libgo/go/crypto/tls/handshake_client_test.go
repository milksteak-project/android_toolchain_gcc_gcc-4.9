// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package tls

import (
	"bytes"
	"flag"
	"io"
	"net"
	"os"
	"testing"
)

func testClientScript(t *testing.T, name string, clientScript [][]byte, config *Config) {
	c, s := net.Pipe()
	cli := Client(c, config)
	go func() {
		cli.Write([]byte("hello\n"))
		cli.Close()
		c.Close()
	}()

	defer c.Close()
	for i, b := range clientScript {
		if i%2 == 1 {
			s.Write(b)
			continue
		}
		bb := make([]byte, len(b))
		_, err := io.ReadFull(s, bb)
		if err != nil {
			t.Fatalf("%s #%d: %s", name, i, err)
		}
		if !bytes.Equal(b, bb) {
			t.Fatalf("%s #%d: mismatch on read: got:%x want:%x", name, i, bb, b)
		}
	}
}

func TestHandshakeClientRC4(t *testing.T) {
	var config = *testConfig
	config.CipherSuites = []uint16{TLS_RSA_WITH_RC4_128_SHA}
	testClientScript(t, "RC4", rc4ClientScript, &config)
}

func TestHandshakeClientECDHEAES(t *testing.T) {
	var config = *testConfig
	config.CipherSuites = []uint16{TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA}
	testClientScript(t, "ECDHE-AES", ecdheAESClientScript, &config)
}

var connect = flag.Bool("connect", false, "connect to a TLS server on :10443")

func TestRunClient(t *testing.T) {
	if !*connect {
		return
	}

	tcpConn, err := net.Dial("tcp", "127.0.0.1:10443")
	if err != nil {
		t.Fatal(err)
	}

	record := &recordingConn{
		Conn: tcpConn,
	}

	config := GetTestConfig()
	conn := Client(record, config)
	if err := conn.Handshake(); err != nil {
		t.Fatalf("error from TLS handshake: %s", err)
	}

	conn.Write([]byte("hello\n"))
	conn.Close()

	record.WriteTo(os.Stdout)
}

// Script of interaction with gnutls implementation.
// The values for this test are obtained by building and running in client mode:
//   % go test -test.run "TestRunClient" -connect
// The recorded bytes are written to stdout.
//
// The server private key is:
// -----BEGIN RSA PRIVATE KEY-----
// MIIBPAIBAAJBAJ+zw4Qnlf8SMVIPFe9GEcStgOY2Ww/dgNdhjeD8ckUJNP5VZkVD
// TGiXav6ooKXfX3j/7tdkuD8Ey2//Kv7+ue0CAwEAAQJAN6W31vDEP2DjdqhzCDDu
// OA4NACqoiFqyblo7yc2tM4h4xMbC3Yx5UKMN9ZkCtX0gzrz6DyF47bdKcWBzNWCj
// gQIhANEoojVt7hq+SQ6MCN6FTAysGgQf56Q3TYoJMoWvdiXVAiEAw3e3rc+VJpOz
// rHuDo6bgpjUAAXM+v3fcpsfZSNO6V7kCIQCtbVjanpUwvZkMI9by02oUk9taki3b
// PzPfAfNPYAbCJQIhAJXNQDWyqwn/lGmR11cqY2y9nZ1+5w3yHGatLrcDnQHxAiEA
// vnlEGo8K85u+KwIOimM48ZG8oTk7iFdkqLJR1utT3aU=
// -----END RSA PRIVATE KEY-----
//
// and certificate is:
// -----BEGIN CERTIFICATE-----
// MIICKzCCAdWgAwIBAgIJALE1E2URIMWSMA0GCSqGSIb3DQEBBQUAMEUxCzAJBgNV
// BAYTAkFVMRMwEQYDVQQIEwpTb21lLVN0YXRlMSEwHwYDVQQKExhJbnRlcm5ldCBX
// aWRnaXRzIFB0eSBMdGQwHhcNMTIwNDA2MTcxMDEzWhcNMTUwNDA2MTcxMDEzWjBF
// MQswCQYDVQQGEwJBVTETMBEGA1UECBMKU29tZS1TdGF0ZTEhMB8GA1UEChMYSW50
// ZXJuZXQgV2lkZ2l0cyBQdHkgTHRkMFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAJ+z
// w4Qnlf8SMVIPFe9GEcStgOY2Ww/dgNdhjeD8ckUJNP5VZkVDTGiXav6ooKXfX3j/
// 7tdkuD8Ey2//Kv7+ue0CAwEAAaOBpzCBpDAdBgNVHQ4EFgQUeKaXmmO1xaGlM7oi
// fCNuWxt6zCswdQYDVR0jBG4wbIAUeKaXmmO1xaGlM7oifCNuWxt6zCuhSaRHMEUx
// CzAJBgNVBAYTAkFVMRMwEQYDVQQIEwpTb21lLVN0YXRlMSEwHwYDVQQKExhJbnRl
// cm5ldCBXaWRnaXRzIFB0eSBMdGSCCQCxNRNlESDFkjAMBgNVHRMEBTADAQH/MA0G
// CSqGSIb3DQEBBQUAA0EAhTZAc8G7GtrUWZ8tonAxRnTsg26oyDxRrzms7EC86CJG
// HZnWRiok1IsFCEv7NRFukrt3uuQSu/TIXpyBqJdgTA==
// -----END CERTIFICATE-----
var rc4ClientScript = [][]byte{
	{
		0x16, 0x03, 0x01, 0x00, 0x4a, 0x01, 0x00, 0x00,
		0x46, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x05,
		0x01, 0x00, 0x00, 0x1b, 0x00, 0x05, 0x00, 0x05,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
		0x08, 0x00, 0x06, 0x00, 0x17, 0x00, 0x18, 0x00,
		0x19, 0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,
	},

	{
		0x16, 0x03, 0x01, 0x00, 0x4a, 0x02, 0x00, 0x00,
		0x46, 0x03, 0x01, 0x4d, 0x0a, 0x56, 0x16, 0xb5,
		0x91, 0xd1, 0xcb, 0x80, 0x4d, 0xc7, 0x46, 0xf3,
		0x37, 0x0c, 0xef, 0xea, 0x64, 0x11, 0x14, 0x56,
		0x97, 0x9b, 0xc5, 0x67, 0x08, 0xb7, 0x13, 0xea,
		0xf8, 0xc9, 0xb3, 0x20, 0xe2, 0xfc, 0x41, 0xf6,
		0x96, 0x90, 0x9d, 0x43, 0x9b, 0xe9, 0x6e, 0xf8,
		0x41, 0x16, 0xcc, 0xf3, 0xc7, 0xde, 0xda, 0x5a,
		0xa1, 0x33, 0x69, 0xe2, 0xde, 0x5b, 0xaf, 0x2a,
		0x92, 0xe7, 0xd4, 0xa0, 0x00, 0x05, 0x00, 0x16,
		0x03, 0x01, 0x01, 0xf7, 0x0b, 0x00, 0x01, 0xf3,
		0x00, 0x01, 0xf0, 0x00, 0x01, 0xed, 0x30, 0x82,
		0x01, 0xe9, 0x30, 0x82, 0x01, 0x52, 0x02, 0x01,
		0x06, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48,
		0x86, 0xf7, 0x0d, 0x01, 0x01, 0x04, 0x05, 0x00,
		0x30, 0x5b, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03,
		0x55, 0x04, 0x06, 0x13, 0x02, 0x41, 0x55, 0x31,
		0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08,
		0x13, 0x0a, 0x51, 0x75, 0x65, 0x65, 0x6e, 0x73,
		0x6c, 0x61, 0x6e, 0x64, 0x31, 0x1a, 0x30, 0x18,
		0x06, 0x03, 0x55, 0x04, 0x0a, 0x13, 0x11, 0x43,
		0x72, 0x79, 0x70, 0x74, 0x53, 0x6f, 0x66, 0x74,
		0x20, 0x50, 0x74, 0x79, 0x20, 0x4c, 0x74, 0x64,
		0x31, 0x1b, 0x30, 0x19, 0x06, 0x03, 0x55, 0x04,
		0x03, 0x13, 0x12, 0x54, 0x65, 0x73, 0x74, 0x20,
		0x43, 0x41, 0x20, 0x28, 0x31, 0x30, 0x32, 0x34,
		0x20, 0x62, 0x69, 0x74, 0x29, 0x30, 0x1e, 0x17,
		0x0d, 0x30, 0x30, 0x31, 0x30, 0x31, 0x36, 0x32,
		0x32, 0x33, 0x31, 0x30, 0x33, 0x5a, 0x17, 0x0d,
		0x30, 0x33, 0x30, 0x31, 0x31, 0x34, 0x32, 0x32,
		0x33, 0x31, 0x30, 0x33, 0x5a, 0x30, 0x63, 0x31,
		0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06,
		0x13, 0x02, 0x41, 0x55, 0x31, 0x13, 0x30, 0x11,
		0x06, 0x03, 0x55, 0x04, 0x08, 0x13, 0x0a, 0x51,
		0x75, 0x65, 0x65, 0x6e, 0x73, 0x6c, 0x61, 0x6e,
		0x64, 0x31, 0x1a, 0x30, 0x18, 0x06, 0x03, 0x55,
		0x04, 0x0a, 0x13, 0x11, 0x43, 0x72, 0x79, 0x70,
		0x74, 0x53, 0x6f, 0x66, 0x74, 0x20, 0x50, 0x74,
		0x79, 0x20, 0x4c, 0x74, 0x64, 0x31, 0x23, 0x30,
		0x21, 0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x1a,
		0x53, 0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x74,
		0x65, 0x73, 0x74, 0x20, 0x63, 0x65, 0x72, 0x74,
		0x20, 0x28, 0x35, 0x31, 0x32, 0x20, 0x62, 0x69,
		0x74, 0x29, 0x30, 0x5c, 0x30, 0x0d, 0x06, 0x09,
		0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
		0x01, 0x05, 0x00, 0x03, 0x4b, 0x00, 0x30, 0x48,
		0x02, 0x41, 0x00, 0x9f, 0xb3, 0xc3, 0x84, 0x27,
		0x95, 0xff, 0x12, 0x31, 0x52, 0x0f, 0x15, 0xef,
		0x46, 0x11, 0xc4, 0xad, 0x80, 0xe6, 0x36, 0x5b,
		0x0f, 0xdd, 0x80, 0xd7, 0x61, 0x8d, 0xe0, 0xfc,
		0x72, 0x45, 0x09, 0x34, 0xfe, 0x55, 0x66, 0x45,
		0x43, 0x4c, 0x68, 0x97, 0x6a, 0xfe, 0xa8, 0xa0,
		0xa5, 0xdf, 0x5f, 0x78, 0xff, 0xee, 0xd7, 0x64,
		0xb8, 0x3f, 0x04, 0xcb, 0x6f, 0xff, 0x2a, 0xfe,
		0xfe, 0xb9, 0xed, 0x02, 0x03, 0x01, 0x00, 0x01,
		0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
		0xf7, 0x0d, 0x01, 0x01, 0x04, 0x05, 0x00, 0x03,
		0x81, 0x81, 0x00, 0x93, 0xd2, 0x0a, 0xc5, 0x41,
		0xe6, 0x5a, 0xa9, 0x86, 0xf9, 0x11, 0x87, 0xe4,
		0xdb, 0x45, 0xe2, 0xc5, 0x95, 0x78, 0x1a, 0x6c,
		0x80, 0x6d, 0x73, 0x1f, 0xb4, 0x6d, 0x44, 0xa3,
		0xba, 0x86, 0x88, 0xc8, 0x58, 0xcd, 0x1c, 0x06,
		0x35, 0x6c, 0x44, 0x62, 0x88, 0xdf, 0xe4, 0xf6,
		0x64, 0x61, 0x95, 0xef, 0x4a, 0xa6, 0x7f, 0x65,
		0x71, 0xd7, 0x6b, 0x88, 0x39, 0xf6, 0x32, 0xbf,
		0xac, 0x93, 0x67, 0x69, 0x51, 0x8c, 0x93, 0xec,
		0x48, 0x5f, 0xc9, 0xb1, 0x42, 0xf9, 0x55, 0xd2,
		0x7e, 0x4e, 0xf4, 0xf2, 0x21, 0x6b, 0x90, 0x57,
		0xe6, 0xd7, 0x99, 0x9e, 0x41, 0xca, 0x80, 0xbf,
		0x1a, 0x28, 0xa2, 0xca, 0x5b, 0x50, 0x4a, 0xed,
		0x84, 0xe7, 0x82, 0xc7, 0xd2, 0xcf, 0x36, 0x9e,
		0x6a, 0x67, 0xb9, 0x88, 0xa7, 0xf3, 0x8a, 0xd0,
		0x04, 0xf8, 0xe8, 0xc6, 0x17, 0xe3, 0xc5, 0x29,
		0xbc, 0x17, 0xf1, 0x16, 0x03, 0x01, 0x00, 0x04,
		0x0e, 0x00, 0x00, 0x00,
	},

	{
		0x16, 0x03, 0x01, 0x00, 0x46, 0x10, 0x00, 0x00,
		0x42, 0x00, 0x40, 0x87, 0xa1, 0x1f, 0x14, 0xe1,
		0xfb, 0x91, 0xac, 0x58, 0x2e, 0xf3, 0x71, 0xce,
		0x01, 0x85, 0x2c, 0xc7, 0xfe, 0x84, 0x87, 0x82,
		0xb7, 0x57, 0xdb, 0x37, 0x4d, 0x46, 0x83, 0x67,
		0x52, 0x82, 0x51, 0x01, 0x95, 0x23, 0x68, 0x69,
		0x6b, 0xd0, 0xa7, 0xa7, 0xe5, 0x88, 0xd0, 0x47,
		0x71, 0xb8, 0xd2, 0x03, 0x05, 0x25, 0x56, 0x5c,
		0x10, 0x08, 0xc6, 0x9b, 0xd4, 0x67, 0xcd, 0x28,
		0xbe, 0x9c, 0x48, 0x14, 0x03, 0x01, 0x00, 0x01,
		0x01, 0x16, 0x03, 0x01, 0x00, 0x24, 0xc1, 0xb8,
		0xd3, 0x7f, 0xc5, 0xc2, 0x5a, 0x1d, 0x6d, 0x5b,
		0x2d, 0x5c, 0x82, 0x87, 0xc2, 0x6f, 0x0d, 0x63,
		0x7b, 0x72, 0x2b, 0xda, 0x69, 0xc4, 0xfe, 0x3c,
		0x84, 0xa1, 0x5a, 0x62, 0x38, 0x37, 0xc6, 0x54,
		0x25, 0x2a,
	},

	{
		0x14, 0x03, 0x01, 0x00, 0x01, 0x01, 0x16, 0x03,
		0x01, 0x00, 0x24, 0xea, 0x88, 0x9c, 0x00, 0xf6,
		0x35, 0xb8, 0x42, 0x7f, 0x15, 0x17, 0x76, 0x5e,
		0x4b, 0x24, 0xcb, 0x7e, 0xa0, 0x7b, 0xc3, 0x70,
		0x52, 0x0a, 0x88, 0x2a, 0x7a, 0x45, 0x59, 0x90,
		0x59, 0xac, 0xc6, 0xb5, 0x56, 0x55, 0x96,
	},
}

var ecdheAESClientScript = [][]byte{
	{
		0x16, 0x03, 0x01, 0x00, 0x4a, 0x01, 0x00, 0x00,
		0x46, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc0, 0x13,
		0x01, 0x00, 0x00, 0x1b, 0x00, 0x05, 0x00, 0x05,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
		0x08, 0x00, 0x06, 0x00, 0x17, 0x00, 0x18, 0x00,
		0x19, 0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,
	},
	{
		0x16, 0x03, 0x01, 0x00, 0x54, 0x02, 0x00, 0x00,
		0x50, 0x03, 0x01, 0x50, 0x77, 0x31, 0xf7, 0x5b,
		0xdb, 0x3d, 0x7a, 0x62, 0x76, 0x70, 0x95, 0x33,
		0x73, 0x71, 0x13, 0xfe, 0xa3, 0xb1, 0xd8, 0xb3,
		0x4d, 0x0d, 0xdc, 0xfe, 0x58, 0x6e, 0x6a, 0x3a,
		0xf9, 0xde, 0xdc, 0x20, 0x8e, 0xfa, 0x3d, 0x60,
		0xd0, 0xda, 0xa4, 0x0e, 0x36, 0xf0, 0xde, 0xb6,
		0x81, 0xb4, 0x80, 0x5e, 0xf9, 0xd2, 0x4c, 0xec,
		0xd1, 0x9c, 0x2a, 0x81, 0xc3, 0x36, 0x0b, 0x0f,
		0x4a, 0x3d, 0xdf, 0x75, 0xc0, 0x13, 0x00, 0x00,
		0x08, 0x00, 0x0b, 0x00, 0x04, 0x03, 0x00, 0x01,
		0x02, 0x16, 0x03, 0x01, 0x02, 0x39, 0x0b, 0x00,
		0x02, 0x35, 0x00, 0x02, 0x32, 0x00, 0x02, 0x2f,
		0x30, 0x82, 0x02, 0x2b, 0x30, 0x82, 0x01, 0xd5,
		0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x09, 0x00,
		0xb1, 0x35, 0x13, 0x65, 0x11, 0x20, 0xc5, 0x92,
		0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
		0xf7, 0x0d, 0x01, 0x01, 0x05, 0x05, 0x00, 0x30,
		0x45, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55,
		0x04, 0x06, 0x13, 0x02, 0x41, 0x55, 0x31, 0x13,
		0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x13,
		0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74,
		0x61, 0x74, 0x65, 0x31, 0x21, 0x30, 0x1f, 0x06,
		0x03, 0x55, 0x04, 0x0a, 0x13, 0x18, 0x49, 0x6e,
		0x74, 0x65, 0x72, 0x6e, 0x65, 0x74, 0x20, 0x57,
		0x69, 0x64, 0x67, 0x69, 0x74, 0x73, 0x20, 0x50,
		0x74, 0x79, 0x20, 0x4c, 0x74, 0x64, 0x30, 0x1e,
		0x17, 0x0d, 0x31, 0x32, 0x30, 0x34, 0x30, 0x36,
		0x31, 0x37, 0x31, 0x30, 0x31, 0x33, 0x5a, 0x17,
		0x0d, 0x31, 0x35, 0x30, 0x34, 0x30, 0x36, 0x31,
		0x37, 0x31, 0x30, 0x31, 0x33, 0x5a, 0x30, 0x45,
		0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04,
		0x06, 0x13, 0x02, 0x41, 0x55, 0x31, 0x13, 0x30,
		0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x13, 0x0a,
		0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61,
		0x74, 0x65, 0x31, 0x21, 0x30, 0x1f, 0x06, 0x03,
		0x55, 0x04, 0x0a, 0x13, 0x18, 0x49, 0x6e, 0x74,
		0x65, 0x72, 0x6e, 0x65, 0x74, 0x20, 0x57, 0x69,
		0x64, 0x67, 0x69, 0x74, 0x73, 0x20, 0x50, 0x74,
		0x79, 0x20, 0x4c, 0x74, 0x64, 0x30, 0x5c, 0x30,
		0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
		0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x4b,
		0x00, 0x30, 0x48, 0x02, 0x41, 0x00, 0x9f, 0xb3,
		0xc3, 0x84, 0x27, 0x95, 0xff, 0x12, 0x31, 0x52,
		0x0f, 0x15, 0xef, 0x46, 0x11, 0xc4, 0xad, 0x80,
		0xe6, 0x36, 0x5b, 0x0f, 0xdd, 0x80, 0xd7, 0x61,
		0x8d, 0xe0, 0xfc, 0x72, 0x45, 0x09, 0x34, 0xfe,
		0x55, 0x66, 0x45, 0x43, 0x4c, 0x68, 0x97, 0x6a,
		0xfe, 0xa8, 0xa0, 0xa5, 0xdf, 0x5f, 0x78, 0xff,
		0xee, 0xd7, 0x64, 0xb8, 0x3f, 0x04, 0xcb, 0x6f,
		0xff, 0x2a, 0xfe, 0xfe, 0xb9, 0xed, 0x02, 0x03,
		0x01, 0x00, 0x01, 0xa3, 0x81, 0xa7, 0x30, 0x81,
		0xa4, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e,
		0x04, 0x16, 0x04, 0x14, 0x78, 0xa6, 0x97, 0x9a,
		0x63, 0xb5, 0xc5, 0xa1, 0xa5, 0x33, 0xba, 0x22,
		0x7c, 0x23, 0x6e, 0x5b, 0x1b, 0x7a, 0xcc, 0x2b,
		0x30, 0x75, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04,
		0x6e, 0x30, 0x6c, 0x80, 0x14, 0x78, 0xa6, 0x97,
		0x9a, 0x63, 0xb5, 0xc5, 0xa1, 0xa5, 0x33, 0xba,
		0x22, 0x7c, 0x23, 0x6e, 0x5b, 0x1b, 0x7a, 0xcc,
		0x2b, 0xa1, 0x49, 0xa4, 0x47, 0x30, 0x45, 0x31,
		0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06,
		0x13, 0x02, 0x41, 0x55, 0x31, 0x13, 0x30, 0x11,
		0x06, 0x03, 0x55, 0x04, 0x08, 0x13, 0x0a, 0x53,
		0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61, 0x74,
		0x65, 0x31, 0x21, 0x30, 0x1f, 0x06, 0x03, 0x55,
		0x04, 0x0a, 0x13, 0x18, 0x49, 0x6e, 0x74, 0x65,
		0x72, 0x6e, 0x65, 0x74, 0x20, 0x57, 0x69, 0x64,
		0x67, 0x69, 0x74, 0x73, 0x20, 0x50, 0x74, 0x79,
		0x20, 0x4c, 0x74, 0x64, 0x82, 0x09, 0x00, 0xb1,
		0x35, 0x13, 0x65, 0x11, 0x20, 0xc5, 0x92, 0x30,
		0x0c, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x04, 0x05,
		0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0d, 0x06,
		0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
		0x01, 0x05, 0x05, 0x00, 0x03, 0x41, 0x00, 0x85,
		0x36, 0x40, 0x73, 0xc1, 0xbb, 0x1a, 0xda, 0xd4,
		0x59, 0x9f, 0x2d, 0xa2, 0x70, 0x31, 0x46, 0x74,
		0xec, 0x83, 0x6e, 0xa8, 0xc8, 0x3c, 0x51, 0xaf,
		0x39, 0xac, 0xec, 0x40, 0xbc, 0xe8, 0x22, 0x46,
		0x1d, 0x99, 0xd6, 0x46, 0x2a, 0x24, 0xd4, 0x8b,
		0x05, 0x08, 0x4b, 0xfb, 0x35, 0x11, 0x6e, 0x92,
		0xbb, 0x77, 0xba, 0xe4, 0x12, 0xbb, 0xf4, 0xc8,
		0x5e, 0x9c, 0x81, 0xa8, 0x97, 0x60, 0x4c, 0x16,
		0x03, 0x01, 0x00, 0x8b, 0x0c, 0x00, 0x00, 0x87,
		0x03, 0x00, 0x17, 0x41, 0x04, 0xec, 0x06, 0x1f,
		0xa0, 0x5e, 0x29, 0x49, 0x71, 0x8b, 0x04, 0x9f,
		0x47, 0x87, 0xb1, 0xcb, 0xae, 0x57, 0x8f, 0xd7,
		0xf6, 0xf8, 0x59, 0x74, 0x64, 0x5d, 0x3a, 0x08,
		0xaf, 0x20, 0xc6, 0xd9, 0xfc, 0x5e, 0x36, 0x8b,
		0x62, 0x0e, 0xdb, 0xee, 0xd8, 0xcd, 0xef, 0x25,
		0x8a, 0x38, 0x88, 0x2d, 0x5c, 0x71, 0x50, 0x22,
		0xda, 0x3f, 0x94, 0x06, 0xc9, 0x68, 0x5b, 0x78,
		0x3d, 0x95, 0xca, 0x54, 0x44, 0x00, 0x40, 0x36,
		0xcf, 0x10, 0x81, 0xb4, 0x32, 0x45, 0x3c, 0xa5,
		0x2d, 0x3e, 0xb0, 0xf8, 0xf4, 0x51, 0xf5, 0x28,
		0x09, 0x85, 0x71, 0xa6, 0x79, 0x71, 0x4b, 0x4e,
		0xda, 0x32, 0x5a, 0xc7, 0xb3, 0x57, 0xfd, 0xe8,
		0x12, 0xab, 0xd8, 0x29, 0xfb, 0x8b, 0x43, 0x8f,
		0x7e, 0x27, 0x63, 0x91, 0x84, 0x9c, 0x51, 0x0c,
		0x26, 0x7e, 0x36, 0x3b, 0x37, 0x8d, 0x8f, 0x9e,
		0xe2, 0x82, 0x62, 0xbb, 0xe5, 0xdf, 0xfc, 0x16,
		0x03, 0x01, 0x00, 0x04, 0x0e, 0x00, 0x00, 0x00,
	},
	{
		0x16, 0x03, 0x01, 0x00, 0x46, 0x10, 0x00, 0x00,
		0x42, 0x41, 0x04, 0x1e, 0x18, 0x37, 0xef, 0x0d,
		0x19, 0x51, 0x88, 0x35, 0x75, 0x71, 0xb5, 0xe5,
		0x54, 0x5b, 0x12, 0x2e, 0x8f, 0x09, 0x67, 0xfd,
		0xa7, 0x24, 0x20, 0x3e, 0xb2, 0x56, 0x1c, 0xce,
		0x97, 0x28, 0x5e, 0xf8, 0x2b, 0x2d, 0x4f, 0x9e,
		0xf1, 0x07, 0x9f, 0x6c, 0x4b, 0x5b, 0x83, 0x56,
		0xe2, 0x32, 0x42, 0xe9, 0x58, 0xb6, 0xd7, 0x49,
		0xa6, 0xb5, 0x68, 0x1a, 0x41, 0x03, 0x56, 0x6b,
		0xdc, 0x5a, 0x89, 0x14, 0x03, 0x01, 0x00, 0x01,
		0x01, 0x16, 0x03, 0x01, 0x00, 0x30, 0x9a, 0xaa,
		0xca, 0x5b, 0x57, 0xae, 0x34, 0x92, 0x80, 0x45,
		0x7f, 0xe6, 0xf9, 0x09, 0x19, 0xd0, 0xf0, 0x1e,
		0x4b, 0xc3, 0xda, 0x71, 0xce, 0x34, 0x33, 0x56,
		0x9f, 0x20, 0x9f, 0xf9, 0xa8, 0x62, 0x6c, 0x38,
		0x1b, 0x41, 0xf5, 0x54, 0xf2, 0x79, 0x42, 0x6c,
		0xb5, 0x0e, 0xe7, 0xe1, 0xbc, 0x54,
	},
	{
		0x14, 0x03, 0x01, 0x00, 0x01, 0x01, 0x16, 0x03,
		0x01, 0x00, 0x30, 0x62, 0x82, 0x41, 0x75, 0x2b,
		0xee, 0x0f, 0xdc, 0x6c, 0x48, 0x5a, 0x63, 0xd6,
		0xcb, 0x0a, 0xfd, 0x0a, 0x0e, 0xde, 0x8b, 0x41,
		0x19, 0x0c, 0x13, 0x6b, 0x12, 0xd1, 0xc2, 0x53,
		0xeb, 0x1e, 0xf3, 0x7a, 0xbf, 0x23, 0xc5, 0xa6,
		0x81, 0xa1, 0xdb, 0xab, 0x2f, 0x2c, 0xbc, 0x35,
		0x96, 0x72, 0x83,
	},
	{
		0x17, 0x03, 0x01, 0x00, 0x20, 0xaf, 0x5d, 0x35,
		0x57, 0x10, 0x60, 0xb3, 0x25, 0x7c, 0x26, 0x0f,
		0xf3, 0x5e, 0xb3, 0x0d, 0xad, 0x14, 0x53, 0xcc,
		0x0c, 0x08, 0xd9, 0xa2, 0x67, 0xab, 0xf4, 0x03,
		0x17, 0x20, 0xf1, 0x7e, 0xca, 0x15, 0x03, 0x01,
		0x00, 0x20, 0x30, 0xd0, 0xc1, 0xfb, 0x5f, 0xa6,
		0x1b, 0xb4, 0x48, 0xc2, 0x0b, 0x98, 0xa8, 0x88,
		0x7a, 0xba, 0xdf, 0x36, 0x06, 0xd8, 0xcc, 0xe9,
		0x34, 0xdd, 0x64, 0xc8, 0x73, 0xc5, 0xa2, 0x34,
		0x64, 0xb7,
	},
}
