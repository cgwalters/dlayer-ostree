# dlayer-ostree

Import Docker images into an OSTree repository, and check them out.

## Getting started

```
# No need to be root
$ id -u
1000
$ cd ~/tmp
$ mkdir repo && ostree --repo=repo init --mode=bare-user
$ env DEBUG=1 ~/bin/docker-fetch cgwalters/alpine-test
$ ostree --repo=repo refs
dockerimg/695b0cde8e9d4fb2ce79f9051f3a459aa9a2453ec2164a2eae7ac5851bb9032c
dockerimg/764d96be9c1b5eeba90d957ed8850be41b8e63264ac155a0127d0f0f60bc6889
dockerimg/498bbb75c2784f6721aceb6ee6105a4859458fcdc78b7b974b369a6447b7a68b
dockerimg/6af86e2fec1d829300d9e458c3db79562d0ad30b0a6c08229f186f13f3798d53
dockerimg/463737dfe56d0d8095f81fbd6a67312bc88c179f59face13739bcb4b39a769a9
$ dlayer-ostree --repo=repo checkout -U 695b0cde8e9d4fb2ce79f9051f3a459aa9a2453ec2164a2eae7ac5851bb9032c 695b0cde8e9d4fb2ce79f9051f3a459aa9a2453ec2164a2eae7ac5851bb9032c
$ ls -al 695b0cde8e9d4fb2ce79f9051f3a459aa9a2453ec2164a2eae7ac5851bb9032c
```


