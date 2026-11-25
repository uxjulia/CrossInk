"""Use the simulator TLS adapter without modifying the SDK implementation."""
Import("env")

# The simulator library supplies the same SecureClient API using host OpenSSL.
# Omit the SDK implementation only in the simulator environments that load us.
env.AddBuildMiddleware(lambda env, node: None, "*/SecureNet/src/SecureClient.cpp")
