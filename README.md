# optimizing-io-for-webservers
This research project aims to optimize input output requests and analyze performance on existing webservers.


A Server at minimum needs to:

- Create a socket
- Bind the socket to an address
- Listen on the address
- Block on Accept until a connection is made
- Read on the connected socket
- Figure out how to respond
- Write back on the connected socket
- Close the connection
- Go back to blocking on Accept
