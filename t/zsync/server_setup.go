package main

import (
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var (
	// Global variables for server setup
	apacheProcess *exec.Cmd
	apacheConfig  *os.File
	proxyProcess  *exec.Cmd
	testDir       = ".."
)

// SetupApacheServer sets up and starts the Apache HTTP server for testing
func SetupApacheServer() error {
	if err := os.Mkdir(filepath.Join(testDir, "logs"), 0750); err != nil && !os.IsExist(err) {
		return fmt.Errorf("failed to make logs directory: %v", err)
	}
	if _, err := os.Stat(filepath.Join(testDir, "selfsigned.crt")); os.IsNotExist(err) {
		err = CreateSSLCertificate(filepath.Join(testDir, "selfsigned.crt"), filepath.Join(testDir, "selfsigned.key"))
		if err != nil {
			return fmt.Errorf("failed creating self-signed certificate: %v", err)
		}
	}
	// Read template config
	templatePath := filepath.Join(testDir, "apache.conf.template")
	templateContent, err := os.ReadFile(templatePath)
	if err != nil {
		return fmt.Errorf("failed to read apache config template: %w", err)
	}

	// Get absolute path of test directory
	absTestDir, err := filepath.Abs(testDir)
	if err != nil {
		return fmt.Errorf("failed to get absolute path: %w", err)
	}

	// Replace placeholder with actual directory
	configContent := strings.ReplaceAll(string(templateContent), "%DIR%", absTestDir)

	// Write config to temporary file
	apacheConfig, err = os.CreateTemp("", "apache-*.conf")
	if err != nil {
		return fmt.Errorf("failed to create temp config file: %w", err)
	}

	if _, err := apacheConfig.WriteString(configContent); err != nil {
		return fmt.Errorf("failed to write apache config: %w", err)
	}
	if err := apacheConfig.Close(); err != nil {
		return fmt.Errorf("failed to write apache config: %w", err)
	}

	// Find Apache executable
	apachePath := findApacheExecutable()
	if apachePath == "" {
		return fmt.Errorf("apache not found in /usr/sbin")
	}

	// Start Apache
	apacheProcess = exec.Command(apachePath, "-f", apacheConfig.Name(), "-X")
	if err := apacheProcess.Start(); err != nil {
		return fmt.Errorf("failed to start apache: %w", err)
	}

	// Wait for Apache to be ready with timeout
	if err := WaitForServer("http://localhost:8081/", 10*time.Second); err != nil {
		_ = apacheProcess.Process.Kill()
		_ = apacheProcess.Wait()
		return fmt.Errorf("apache failed to start: %w", err)
	}

	return nil
}

// TeardownApacheServer stops the Apache server
func TeardownApacheServer() error {
	if apacheProcess != nil && apacheProcess.Process != nil {
		if err := apacheProcess.Process.Signal(os.Interrupt); err != nil {
			apacheProcess.Process.Kill()
		}
		_ = apacheProcess.Wait()
	}

	if apacheConfig != nil {
		_ = os.Remove(apacheConfig.Name())
	}

	return nil
}

// SetupProxyServer sets up and starts tinyproxy for testing
func SetupProxyServer(t *testing.T) error {
	configPath := filepath.Join(testDir, "tinyproxy.conf")

	proxyProcess = exec.Command("/usr/sbin/tinyproxy", "-c", configPath, "-d")
	if err := proxyProcess.Start(); err != nil {
		return fmt.Errorf("failed to start tinyproxy: %w", err)
	}

	time.Sleep(1 * time.Second)
	t.Log("Tinyproxy server started successfully")
	return nil
}

// TeardownProxyServer stops the proxy server
func TeardownProxyServer() error {
	if proxyProcess != nil && proxyProcess.Process != nil {
		if err := proxyProcess.Process.Signal(os.Interrupt); err != nil {
			proxyProcess.Process.Kill()
		}
		proxyProcess.Wait()
	}

	return nil
}

// findApacheExecutable finds the Apache executable in common locations
func findApacheExecutable() string {
	candidates := []string{"/usr/sbin/httpd", "/usr/sbin/apache2"}

	for _, path := range candidates {
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}

	return ""
}

// CreateSSLCertificate creates self-signed SSL certificate for testing
func CreateSSLCertificate(certPath, keyPath string) error {
	cmd := exec.Command("openssl", "req", "-x509", "-newkey", "rsa:2048",
		"-keyout", keyPath, "-out", certPath, "-days", "1", "-nodes",
		"-subj", "/C=US/ST=Test/L=Test/O=Test/CN=localhost")

	return cmd.Run()
}

// WaitForServer waits for a server to be ready with timeout
func WaitForServer(url string, timeout time.Duration) error {
	start := time.Now()

	for {
		resp, err := http.Get(url)
		if err == nil {
			resp.Body.Close()
			return nil
		}

		if time.Since(start) > timeout {
			return fmt.Errorf("server did not become ready within %v", timeout)
		}

		time.Sleep(100 * time.Millisecond)
	}
}
