//go:build windows

package main

import "syscall"

// On Windows SO_REUSEADDR lets another program take the port over: leave the socket exclusive
func reuseAddr(network, address string, c syscall.RawConn) error { return nil }
