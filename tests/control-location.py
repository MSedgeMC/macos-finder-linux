# SPDX-License-Identifier: MIT
"""Locate original vector traffic lights, allowing gradients and scale."""
def red_anchor(image):
    pixels=image.convert('RGB').load()
    red=set()
    for y in range(image.height):
        for x in range(image.width):
            r,g,b=pixels[x,y]
            if r>175 and 25<g<155 and b<150 and r>g*1.55 and r>b*1.65:red.add((x,y))
    candidates=[]
    while red:
        point=red.pop();stack=[point];component=[point]
        while stack:
            x,y=stack.pop()
            for neighbor in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                if neighbor in red:red.remove(neighbor);stack.append(neighbor);component.append(neighbor)
        xs=[p[0] for p in component];ys=[p[1] for p in component]
        width=max(xs)-min(xs)+1;height=max(ys)-min(ys)+1
        if 4<=width<=38 and 4<=height<=38 and 15<=len(component)<=1000:
            # Reject unrelated red thumbnail pixels: controls have yellow and
            # green neighbors at the same height with matching spacing.
            cx=(min(xs)+max(xs))//2;cy=(min(ys)+max(ys))//2
            for spacing in range(max(12,width),min(65,width*3+5)):
                if cx+spacing*2>=image.width:break
                yr,yg,yb=pixels[cx+spacing,cy];gr,gg,gb=pixels[cx+spacing*2,cy]
                if yr>140 and yg>95 and yb<155 and gg>110 and gg>gr*1.4 and gg>gb*1.15:
                    candidates.append((min(xs),min(ys)));break
    assert candidates,'traffic-light cluster not visible'
    return min(candidates,key=lambda p:(p[1],p[0]))
def control_origin(client,screen):
    a=red_anchor(client);b=red_anchor(screen)
    rgba=client.convert('RGBA');rgb=screen.convert('RGB')
    samples=[]
    for y in range(max(0,a[1]-2),min(client.height,a[1]+20)):
        for x in range(max(0,a[0]-2),min(client.width,a[0]+20)):
            r,g,bv,alpha=rgba.getpixel((x,y))
            if alpha==255 and r>100 and r>g*1.5 and r>bv*1.6:samples.append((x,y,(r,g,bv)))
    assert samples,'opaque control samples missing'
    coarse=(b[0]-a[0],b[1]-a[1]);scores=[]
    # Antialiased rims are backdrop-dependent. Match the opaque gradient
    # interior to recover the true origin instead of trusting a color cutoff.
    for dx in range(coarse[0]-4,coarse[0]+5):
        for dy in range(coarse[1]-4,coarse[1]+5):
            if any(not(0<=x+dx<screen.width and 0<=y+dy<screen.height) for x,y,_ in samples):continue
            score=sum(sum(abs(v-w) for v,w in zip(color,rgb.getpixel((x+dx,y+dy)))) for x,y,color in samples)
            scores.append((score,dx,dy))
    assert scores,'control matching region outside screen'
    score,dx,dy=min(scores)
    assert score/len(samples)<12,('control match is ambiguous',score,len(samples))
    return dx,dy
